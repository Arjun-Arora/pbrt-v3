#include "lambda-worker.h"

#include <getopt.h>
#include <glog/logging.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "cloud/bvh.h"
#include "cloud/integrator.h"
#include "cloud/lambda-master.h"
#include "cloud/manager.h"
#include "cloud/raystate.h"
#include "cloud/stats.h"
#include "core/camera.h"
#include "core/geometry.h"
#include "core/light.h"
#include "core/sampler.h"
#include "core/spectrum.h"
#include "core/transform.h"
#include "execution/loop.h"
#include "execution/meow/message.h"
#include "messages/utils.h"
#include "net/address.h"
#include "net/requests.h"
#include "net/util.h"
#include "storage/backend.h"
#include "util/exception.h"
#include "util/path.h"
#include "util/random.h"
#include "util/system_runner.h"
#include "util/temp_dir.h"
#include "util/temp_file.h"

using namespace std;
using namespace chrono;
using namespace meow;
using namespace pbrt;
using namespace pbrt::global;
using namespace PollerShortNames;

using OpCode = Message::OpCode;
using PollerResult = Poller::Result::Type;

constexpr size_t UDP_MTU_BYTES{1'350};
constexpr char LOG_STREAM_ENVAR[] = "AWS_LAMBDA_LOG_STREAM_NAME";

#define TLOG(tag) LOG(INFO) << "[" #tag "] "

LambdaWorker::LambdaWorker(const string& coordinatorIP,
                           const uint16_t coordinatorPort,
                           const string& storageUri,
                           const WorkerConfiguration& config)
    : config(config),
      coordinatorAddr(coordinatorIP, coordinatorPort),
      workingDirectory("/tmp/pbrt-worker"),
      storageBackend(StorageBackend::create_backend(storageUri)) {
    cerr << "* starting worker in " << workingDirectory.name() << endl;
    roost::chdir(workingDirectory.name());

    FLAGS_log_dir = ".";
    FLAGS_log_prefix = false;
    google::InitGoogleLogging(logBase.c_str());

    TLOG(DIAG) << "start "
               << duration_cast<microseconds>(
                      workerDiagnostics.startTime.time_since_epoch())
                      .count();

    if (trackRays) {
        TLOG(RAY)
            << "x,y,sample,bounce,hop,tick,shadowRay,workerID,otherPartyID,"
               "treeletID,timestamp,size,action";
    }

    if (trackPackets) {
        TLOG(PACKET) << "sourceID,destinationID,seqNo,attempt,size,"
                        "rayCount,timestamp,action";
    }

    udpConnection.emplace_back(true, config.maxUdpRate);
    udpConnection.emplace_back(true, config.maxUdpRate);

    PbrtOptions.nThreads = 1;
    bvh = make_shared<CloudBVH>();
    manager.init(".");

    srand(time(nullptr));
    do {
        mySeed = rand();
    } while (mySeed == 0);

    coordinatorConnection = loop.make_connection<TCPConnection>(
        coordinatorAddr,
        [this](shared_ptr<TCPConnection>, string&& data) {
            RECORD_INTERVAL("parseTCP");
            this->tcpMessageParser.parse(data);

            while (!this->tcpMessageParser.empty()) {
                this->messageParser.push(move(this->tcpMessageParser.front()));
                this->tcpMessageParser.pop();
            }

            return true;
        },
        []() { LOG(INFO) << "Connection to coordinator failed."; },
        [this]() { this->terminate(); });

    eventAction[Event::UdpReceive] = loop.poller().add_action(
        Poller::Action(udpConnection[0].socket(), Direction::In,
                       bind(&LambdaWorker::handleUdpReceive, this, 0),
                       [this]() { return true; },
                       []() { throw runtime_error("udp in failed"); }));

    eventAction[Event::UdpReceive2] = loop.poller().add_action(
        Poller::Action(udpConnection[1].socket(), Direction::In,
                       bind(&LambdaWorker::handleUdpReceive, this, 1),
                       [this]() { return true; },
                       []() { throw runtime_error("udp in failed"); }));

    eventAction[Event::RayAcks] = loop.poller().add_action(Poller::Action(
        handleRayAcknowledgementsTimer.fd, Direction::In,
        bind(&LambdaWorker::handleRayAcknowledgements, this),
        [this]() {
            return !toBeAcked.empty() ||
                   (!receivedAcks.empty() && !outstandingRayPackets.empty() &&
                    outstandingRayPackets.front().first <= packet_clock::now());
        },
        []() { throw runtime_error("acks failed"); }));

    eventAction[Event::UdpSend] = loop.poller().add_action(Poller::Action(
        udpConnection[0].socket(), Direction::Out,
        bind(&LambdaWorker::handleUdpSend, this, 0),
        [this]() {
            return (!servicePackets.empty() || !rayPackets.empty()) &&
                   udpConnection[0].within_pace();
        },
        []() { throw runtime_error("udp out failed"); }));

    eventAction[Event::UdpSend2] = loop.poller().add_action(Poller::Action(
        udpConnection[1].socket(), Direction::Out,
        bind(&LambdaWorker::handleUdpSend, this, 1),
        [this]() {
            return !servicePackets.empty() && udpConnection[1].within_pace();
        },
        []() { throw runtime_error("udp out failed"); }));

    /* trace rays */
    eventAction[Event::RayQueue] = loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out, bind(&LambdaWorker::handleRayQueue, this),
        [this]() { return !rayQueue.empty(); },
        []() { throw runtime_error("ray queue failed"); }));

    /* send processed rays */
    eventAction[Event::OutQueue] = loop.poller().add_action(
        Poller::Action(outQueueTimer.fd, Direction::In,
                       bind(&LambdaWorker::handleOutQueue, this),
                       [this]() { return outQueueSize > 0; },
                       []() { throw runtime_error("out queue failed"); }));

    /* send finished rays */
    /* FIXME we're throwing out finished rays, for now */
    eventAction[Event::FinishedQueue] = loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out, bind(&LambdaWorker::handleFinishedQueue, this),
        [this]() {
            // clang-format off
            switch (this->config.finishedRayAction) {
            case FinishedRayAction::Discard: return finishedQueue.size() > 5000;
            case FinishedRayAction::SendBack: return !finishedQueue.empty();
            default: return false;
            }
            // clang-format on
        },
        []() { throw runtime_error("finished queue failed"); }));

    /* handle peers */
    eventAction[Event::Peers] = loop.poller().add_action(Poller::Action(
        peerTimer.fd, Direction::In, bind(&LambdaWorker::handlePeers, this),
        [this]() { return !peers.empty(); },
        []() { throw runtime_error("peers failed"); }));

    /* handle received messages */
    eventAction[Event::Messages] = loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out, bind(&LambdaWorker::handleMessages, this),
        [this]() { return !messageParser.empty(); },
        []() { throw runtime_error("messages failed"); }));

    /* send updated stats */
    eventAction[Event::WorkerStats] = loop.poller().add_action(Poller::Action(
        workerStatsTimer.fd, Direction::In,
        bind(&LambdaWorker::handleWorkerStats, this), [this]() { return true; },
        []() { throw runtime_error("worker stats failed"); }));

    /* record diagnostics */
    eventAction[Event::Diagnostics] = loop.poller().add_action(Poller::Action(
        workerDiagnosticsTimer.fd, Direction::In,
        bind(&LambdaWorker::handleDiagnostics, this), [this]() { return true; },
        []() { throw runtime_error("handle diagnostics failed"); }));

    /* request new peers for neighboring treelets */
    /* loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out,
        bind(&LambdaWorker::handleNeededTreelets, this),
        [this]() { return !neededTreelets.empty(); },
        []() { throw runtime_error("treelet request failed"); })); */

    /* send back finished paths */
    /* loop.poller().add_action(
        Poller::Action(finishedPathsTimer.fd, Direction::In,
                       bind(&LambdaWorker::handleFinishedPaths, this),
                       [this]() { return !finishedPathIds.empty(); },
                       []() { throw runtime_error("finished paths failed");
       }));*/

    coordinatorConnection->enqueue_write(
        Message::str(0, OpCode::Hey, safe_getenv_or(LOG_STREAM_ENVAR, "")));
}

void LambdaWorker::NetStats::merge(const NetStats& other) {
    bytesSent += other.bytesSent;
    bytesReceived += other.bytesReceived;
    packetsSent += other.packetsSent;
    packetsReceived += other.packetsReceived;
}

void LambdaWorker::initBenchmark(const uint32_t duration,
                                 const uint32_t destination,
                                 const uint32_t rate,
                                 const uint32_t addressNo) {
    /* (1) disable all unnecessary actions */
    set<uint64_t> toDeactivate{
        eventAction[Event::RayQueue],       eventAction[Event::OutQueue],
        eventAction[Event::FinishedQueue],  eventAction[Event::Peers],
        eventAction[Event::NeededTreelets], eventAction[Event::UdpSend],
        eventAction[Event::UdpReceive],     eventAction[Event::RayAcks],
        eventAction[Event::Diagnostics],    eventAction[Event::WorkerStats],
        eventAction[Event::UdpSend2],       eventAction[Event::UdpReceive2],
    };

    loop.poller().deactivate_actions(toDeactivate);
    udpConnection[0].reset_reference();
    udpConnection[1].reset_reference();

    const uint32_t sendIface = addressNo;
    const uint32_t recvIface = 1 - addressNo;

    if (rate) {
        udpConnection[recvIface].set_rate(rate);
    }

    auto recvCallback = [this, recvIface](const uint32_t iface) {
        auto datagram = udpConnection[iface].socket().recvfrom();

        if (iface == recvIface) {
            benchmarkData.checkpoint.bytesReceived += datagram.second.length();
            benchmarkData.checkpoint.packetsReceived++;
        }

        return ResultType::Continue;
    };

    auto sendCallback = [this, sendIface](const uint32_t iface, const Address address) {
        const static string packet =
            Message::str(*workerId, OpCode::Ping, string(1300, 'x'));
        udpConnection[iface].socket().sendto(address, packet);
        udpConnection[iface].record_send(packet.length());

        if (iface == sendIface) {
            benchmarkData.checkpoint.bytesSent += packet.length();
            benchmarkData.checkpoint.packetsSent++;
        }

        return ResultType::Continue;
    };

    /* (2) set up new udpReceive and udpSend actions */
    eventAction[Event::UdpReceive] = loop.poller().add_action(
        Poller::Action(udpConnection[0].socket(), Direction::In,
                       bind(recvCallback, 0), [this]() { return true; },
                       []() { throw runtime_error("udp in failed"); }));

    eventAction[Event::UdpReceive2] = loop.poller().add_action(
        Poller::Action(udpConnection[1].socket(), Direction::In,
                       bind(recvCallback, 1), [this]() { return true; },
                       []() { throw runtime_error("udp in failed"); }));

    eventAction[Event::UdpSend] = loop.poller().add_action(
        Poller::Action(udpConnection[0].socket(), Direction::Out,
                       bind(sendCallback, 0, peers.at(destination).address[0]),
                       [this]() { return udpConnection[0].within_pace(); },
                       []() { throw runtime_error("udp out failed"); }));

    eventAction[Event::UdpSend2] = loop.poller().add_action(
        Poller::Action(udpConnection[1].socket(), Direction::Out,
                       bind(sendCallback, 1, peers.at(destination).address[1]),
                       [this]() { return udpConnection[1].within_pace(); },
                       []() { throw runtime_error("udp out failed"); }));

    benchmarkTimer = make_unique<TimerFD>(seconds{duration});
    checkpointTimer = make_unique<TimerFD>(seconds{1});

    loop.poller().add_action(Poller::Action(
        benchmarkTimer->fd, Direction::In,
        [this, destination]() {
            benchmarkTimer->reset();
            benchmarkData.end = probe_clock::now();

            set<uint64_t> toDeactivate{
                eventAction[Event::UdpReceive], eventAction[Event::UdpSend],
                eventAction[Event::UdpReceive2], eventAction[Event::UdpSend2],
                eventAction[Event::NetStats]};

            loop.poller().deactivate_actions(toDeactivate);

            return ResultType::CancelAll;
        },
        [this]() { return true; },
        []() { throw runtime_error("benchmark timer failed"); }));

    eventAction[Event::NetStats] = loop.poller().add_action(Poller::Action(
        checkpointTimer->fd, Direction::In,
        [this]() {
            checkpointTimer->reset();
            benchmarkData.checkpoint.timestamp = probe_clock::now();
            benchmarkData.checkpoints.push_back(benchmarkData.checkpoint);
            benchmarkData.stats.merge(benchmarkData.checkpoint);
            benchmarkData.checkpoint = {};

            return ResultType::Continue;
        },
        [this]() { return true; },
        []() { throw runtime_error("net stats failed"); }));

    benchmarkData.start = probe_clock::now();
    benchmarkData.checkpoint.timestamp = benchmarkData.start;
}

Message LambdaWorker::createConnectionRequest(const Worker& peer,
                                              const uint32_t addressNo) {
    protobuf::ConnectRequest proto;
    proto.set_worker_id(*workerId);
    proto.set_my_seed(mySeed);
    proto.set_your_seed(peer.seed);
    proto.set_address_no(addressNo);
    return {*workerId, OpCode::ConnectionRequest, protoutil::to_string(proto)};
}

Message LambdaWorker::createConnectionResponse(const Worker& peer,
                                               const uint32_t addressNo) {
    protobuf::ConnectResponse proto;
    proto.set_worker_id(*workerId);
    proto.set_my_seed(mySeed);
    proto.set_your_seed(peer.seed);
    proto.set_address_no(addressNo);
    for (const auto& treeletId : treeletIds) {
        proto.add_treelet_ids(treeletId);
    }
    return {*workerId, OpCode::ConnectionResponse, protoutil::to_string(proto)};
}

void LambdaWorker::logPacket(const uint64_t sequenceNumber,
                             const uint16_t attempt, const PacketAction action,
                             const WorkerId otherParty, const size_t packetSize,
                             const size_t numRays) {
    if (!trackPackets) return;

    ostringstream oss;

    switch (action) {
    case PacketAction::Queued:
    case PacketAction::Sent:
    case PacketAction::Acked:
    case PacketAction::AckSent:
        oss << *workerId << ',' << otherParty << ',';
        break;

    case PacketAction::Received:
    case PacketAction::AckReceived:
        oss << otherParty << ',' << *workerId << ',';
        break;

    default:
        throw runtime_error("invalid packet action");
    }

    oss << sequenceNumber << ',' << attempt << ',' << packetSize << ','
        << numRays << ','
        << duration_cast<microseconds>(rays_clock::now().time_since_epoch())
               .count()
        << ',';

    // clang-format off
    switch (action) {
    case PacketAction::Queued:      oss << "Queued";      break;
    case PacketAction::Sent:        oss << "Sent";        break;
    case PacketAction::Received:    oss << "Received";    break;
    case PacketAction::Acked:       oss << "Acked";       break;
    case PacketAction::AckSent:     oss << "AckSent";     break;
    case PacketAction::AckReceived: oss << "AckReceived"; break;

    default: throw runtime_error("invalid packet action");
    }
    // clang-format on

    TLOG(PACKET) << oss.str();
}

void LambdaWorker::logRayAction(const RayState& state, const RayAction action,
                                const WorkerId otherParty) {
    if (!trackRays || !state.trackRay) return;

    ostringstream oss;

    // clang-format off
    // x,y,sample,bounce,hop,tick,shadowRay,workerID,otherPartyID,treeletID,timestamp,size,action
    oss << state.sample.pixel.x << ','
        << state.sample.pixel.y << ','
        << state.sample.num << ','
        << (maxDepth - state.remainingBounces) << ','
        << state.hop << ','
        << state.tick << ','
        << state.isShadowRay << ','
        << *workerId << ','
        << ((action == RayAction::Sent ||
             action == RayAction::Received) ? otherParty
                                            : *workerId) << ','
        << state.CurrentTreelet() << ','
        << duration_cast<microseconds>(
               rays_clock::now().time_since_epoch()).count() << ','
        << state.Size() << ',';
    // clang-format on

    // clang-format off
    switch (action) {
    case RayAction::Generated: oss << "Generated"; break;
    case RayAction::Traced:    oss << "Traced";    break;
    case RayAction::Queued:    oss << "Queued";    break;
    case RayAction::Pending:   oss << "Pending";   break;
    case RayAction::Sent:      oss << "Sent";      break;
    case RayAction::Received:  oss << "Received";  break;
    case RayAction::Finished:  oss << "Finished";  break;

    default: throw runtime_error("invalid ray action");
    }
    // clang-format on

    TLOG(RAY) << oss.str();
}

ResultType LambdaWorker::handleRayQueue() {
    RECORD_INTERVAL("handleRayQueue");

    auto recordFinishedPath = [this](const uint64_t pathId) {
        this->workerStats.recordFinishedPath();
        this->finishedPathIds.push_back(pathId);
    };

    deque<RayStatePtr> processedRays;

    constexpr size_t MAX_RAYS = 20'000;

    for (size_t i = 0; i < MAX_RAYS && !rayQueue.empty(); i++) {
        RayStatePtr rayPtr = popRayQueue();
        RayState& ray = *rayPtr;

        const uint64_t pathId = ray.PathID();

        logRayAction(ray, RayAction::Traced);

        if (!ray.toVisitEmpty()) {
            const uint32_t rayTreelet = ray.toVisitTop().treelet;
            auto newRayPtr = CloudIntegrator::Trace(move(rayPtr), bvh);
            auto& newRay = *newRayPtr;

            const bool hit = newRay.hit;
            const bool emptyVisit = newRay.toVisitEmpty();

            if (newRay.isShadowRay) {
                if (hit || emptyVisit) {
                    newRay.Ld = hit ? 0.f : newRay.Ld;
                    logRayAction(*newRayPtr, RayAction::Finished);
                    finishedQueue.push_back(move(newRayPtr));
                } else {
                    processedRays.push_back(move(newRayPtr));
                }
            } else if (!emptyVisit || hit) {
                processedRays.push_back(move(newRayPtr));
            } else if (emptyVisit) {
                newRay.Ld = 0.f;
                logRayAction(*newRayPtr, RayAction::Finished);
                finishedQueue.push_back(move(newRayPtr));
                recordFinishedPath(pathId);
            }
        } else if (ray.hit) {
            auto newRays = CloudIntegrator::Shade(move(rayPtr), bvh, lights,
                                                  sampler, arena);

            for (auto& newRay : newRays.first) {
                logRayAction(*newRay, RayAction::Generated);
                processedRays.push_back(move(newRay));
            }

            if (newRays.second) recordFinishedPath(pathId);

            if (newRays.first.empty()) {
                /* rayPtr is not touched if if Shade() returned nothing */
                logRayAction(*rayPtr, RayAction::Finished);
            }
        } else {
            throw runtime_error("invalid ray in ray queue");
        }
    }

    while (!processedRays.empty()) {
        RayStatePtr ray = move(processedRays.front());
        processedRays.pop_front();

        workerStats.recordDemandedRay(*ray);
        const TreeletId nextTreelet = ray->CurrentTreelet();

        if (treeletIds.count(nextTreelet)) {
            pushRayQueue(move(ray));
        } else {
            if (treeletToWorker.count(nextTreelet)) {
                workerStats.recordSendingRay(*ray);
                outQueue[nextTreelet].push_back(move(ray));
                outQueueSize++;
            } else {
                logRayAction(*ray, RayAction::Pending);
                workerStats.recordPendingRay(*ray);
                neededTreelets.insert(nextTreelet);
                pendingQueue[nextTreelet].push_back(move(ray));
                pendingQueueSize++;
            }
        }
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleOutQueue() {
    RECORD_INTERVAL("handleOutQueue");

    outQueueTimer.reset();

    for (auto& q : outQueue) {
        if (q.second.empty()) continue;

        const auto treeletId = q.first;
        auto& outRays = q.second;

        const auto& candidates = treeletToWorker[treeletId];
        const auto& peer =
            peers.at(*random::sample(candidates.begin(), candidates.end()));

        auto& peerSeqNo = sequenceNumbers[peer.address[0]];

        string unpackedRayStr;
        RayStatePtr unpackedRayPtr;

        while (!outRays.empty() || !unpackedRayStr.empty()) {
            ostringstream oss;
            size_t packetLen = 25;
            size_t rayCount = 0;

            vector<unique_ptr<RayState>> trackedRays;

            {
                protobuf::RecordWriter writer{&oss};

                if (!unpackedRayStr.empty()) {
                    rayCount++;
                    packetLen = unpackedRayStr.length() + 4;
                    writer.write(unpackedRayStr);

                    if (unpackedRayPtr->trackRay) {
                        trackedRays.push_back(move(unpackedRayPtr));
                    }

                    unpackedRayStr.clear();
                }

                while (packetLen < UDP_MTU_BYTES && !outRays.empty()) {
                    RayStatePtr ray = move(outRays.front());
                    outRays.pop_front();
                    outQueueSize--;

                    string rayStr = RayState::serialize(ray);
                    logRayAction(*ray, RayAction::Queued);

                    const size_t len = rayStr.length() + 4;

                    if (len + packetLen > UDP_MTU_BYTES) {
                        unpackedRayStr.swap(rayStr);
                        unpackedRayPtr = move(ray);
                        break;
                    }

                    if (ray->trackRay) {
                        trackedRays.push_back(move(ray));
                    }

                    packetLen += len;
                    writer.write(rayStr);
                    rayCount++;
                }
            }

            oss.flush();

            const bool tracked = packetLogBD(randEngine);

            RayPacket rayPacket{
                peer.address[0],
                peer.id,
                treeletId,
                rayCount,
                Message::str(*workerId, OpCode::SendRays, oss.str(),
                             config.sendReliably, peerSeqNo, tracked),
                config.sendReliably,
                peerSeqNo,
                tracked};

            if (tracked) {
                logPacket(peerSeqNo, 0, PacketAction::Queued, peer.id,
                          rayPacket.data().length(), rayCount);
            }

            rayPacket.trackedRays = move(trackedRays);
            rayPackets.emplace_back(move(rayPacket));

            peerSeqNo++;
        }
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleFinishedPaths() {
    RECORD_INTERVAL("handleFinishedPaths");
    finishedPathsTimer.reset();

    string payload;
    for (const auto pathId : finishedPathIds) {
        payload += put_field(pathId);
    }

    finishedPathIds.clear();

    coordinatorConnection->enqueue_write(
        Message::str(*workerId, OpCode::FinishedPaths, payload));

    return ResultType::Continue;
}

ResultType LambdaWorker::handleFinishedQueue() {
    RECORD_INTERVAL("handleFinishedQueue");

    auto createFinishedRay = [](const size_t sampleId, const Point2f& pFilm,
                                const Float weight,
                                const Spectrum L) -> protobuf::FinishedRay {
        protobuf::FinishedRay proto;
        proto.set_sample_id(sampleId);
        *proto.mutable_p_film() = to_protobuf(pFilm);
        proto.set_weight(weight);
        *proto.mutable_l() = to_protobuf(L);
        return proto;
    };

    switch (config.finishedRayAction) {
    case FinishedRayAction::Discard:
        finishedQueue.clear();
        break;

    case FinishedRayAction::SendBack: {
        ostringstream oss;

        {
            protobuf::RecordWriter writer{&oss};

            while (!finishedQueue.empty()) {
                RayStatePtr rayPtr = move(finishedQueue.front());
                RayState& ray = *rayPtr;
                finishedQueue.pop_front();

                Spectrum L{ray.beta * ray.Ld};

                if (L.HasNaNs() || L.y() < -1e-5 || isinf(L.y())) {
                    L = Spectrum(0.f);
                }

                writer.write(createFinishedRay(ray.sample.id, ray.sample.pFilm,
                                               ray.sample.weight, L));
            }
        }

        oss.flush();
        coordinatorConnection->enqueue_write(
            Message::str(*workerId, OpCode::FinishedRays, oss.str()));

        break;
    }

    case FinishedRayAction::Upload:
        break;

    default:
        throw runtime_error("invalid finished ray action");
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handlePeers() {
    RECORD_INTERVAL("handlePeers");
    peerTimer.reset();

    const auto now = packet_clock::now();

    for (auto it = peers.begin(); it != peers.end(); it++) {
        auto& peerId = it->first;
        auto& peer = it->second;

        switch (peer.state) {
        case Worker::State::Connecting: {
            for (size_t i = 0; i < 2; i++) {
                auto message = createConnectionRequest(peer, i);
                servicePackets.emplace_front(peer.address[i], peer.id,
                                             message.str());
                servicePackets.front().iface = i;
            }

            peer.tries++;
            break;
        }

        case Worker::State::Connected:
            /* send keep alive */
            if (peerId > 0 && peer.nextKeepAlive < now) {
                peer.nextKeepAlive += KEEP_ALIVE_INTERVAL;
                servicePackets.emplace_back(
                    peer.address[0], peer.id,
                    Message::str(*workerId, OpCode::Ping,
                                 put_field(*workerId)));
            }

            break;
        }
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleMessages() {
    RECORD_INTERVAL("handleMessages");
    MessageParser unprocessedMessages;
    while (!messageParser.empty()) {
        Message message = move(messageParser.front());
        messageParser.pop();

        if (!processMessage(message)) {
            unprocessedMessages.push(move(message));
        }
    }

    swap(messageParser, unprocessedMessages);

    return ResultType::Continue;
}

ResultType LambdaWorker::handleNeededTreelets() {
    RECORD_INTERVAL("handleNeededTreelets");
    for (const auto& treeletId : neededTreelets) {
        if (requestedTreelets.count(treeletId)) {
            continue;
        }

        protobuf::GetWorker proto;
        proto.set_treelet_id(treeletId);
        Message message(*workerId, OpCode::GetWorker,
                        protoutil::to_string(proto));
        coordinatorConnection->enqueue_write(message.str());
        requestedTreelets.insert(treeletId);
    }

    neededTreelets.clear();
    return ResultType::Continue;
}

ResultType LambdaWorker::handleRayAcknowledgements() {
    handleRayAcknowledgementsTimer.reset();

    // sending acknowledgements
    for (auto& receivedKv : toBeAcked) {
        auto& received = receivedKv.second;
        string ack;
        const auto destId = addressToWorker[receivedKv.first];

        for (size_t i = 0; i < received.size(); i++) {
            ack += put_field(get<0>(received[i]));
            ack += put_field(get<1>(received[i]));
            ack += put_field(get<2>(received[i]));

            if (ack.length() >= UDP_MTU_BYTES or i == received.size() - 1) {
                const auto myAckId = ackId++;
                const bool tracked = packetLogBD(randEngine);

                Message msg(*workerId, OpCode::Ack, move(ack), false, myAckId,
                            tracked);

                ServicePacket servicePacket(receivedKv.first, destId,
                                            move(msg.str()), true, myAckId,
                                            tracked);

                servicePackets.push_back(move(servicePacket));

                ack = {};
            }
        }
    }

    toBeAcked.clear();

    // retransmit outstanding packets
    const auto now = packet_clock::now();

    while (!receivedAcks.empty() && !outstandingRayPackets.empty() &&
           outstandingRayPackets.front().first <= now) {
        auto& packet = outstandingRayPackets.front().second;
        auto& thisReceivedAcks = receivedAcks[packet.destination];

        if (!thisReceivedAcks.contains(packet.sequenceNumber)) {
            if (false && packet.attempt > 1) {
                /* sequenceNumbers[packet.destination]-- */;

                const auto& candidates = treeletToWorker[packet.targetTreelet];
                const auto& peer = peers.at(
                    *random::sample(candidates.begin(), candidates.end()));

                packet.destination = peer.address[0];
                packet.destinationId = peer.id;
                packet.sequenceNumber = sequenceNumbers[packet.destination]++;
                packet.attempt = 0;
            }

            packet.incrementAttempts();
            packet.retransmission = true;
            rayPackets.push_back(move(packet));
        }

        outstandingRayPackets.pop_front();
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleUdpSend(const size_t iface) {
    RECORD_INTERVAL("sendUDP");

    cout << "sendUdp" << endl;

    /* we always send service packets first */
    auto sendUdpPacket = [this](const Address& peer, const string& payload,
                                const size_t iface) {
        cout << "sending to " << peer.str() << " " << iface;
        udpConnection[iface].bytes_sent += payload.length();
        udpConnection[iface].socket().sendto(peer, payload);
        udpConnection[iface].record_send(payload.length());
        cout << "sending done" << endl;
    };

    for (auto datagramIt = servicePackets.begin();
         datagramIt != servicePackets.end(); datagramIt++) {
        auto& datagram = *datagramIt;

        if (datagram.iface != iface) continue;

        sendUdpPacket(datagram.destination, datagram.data, datagram.iface);

        if (datagram.ackPacket && datagram.tracked) {
            logPacket(datagram.ackId, 0, PacketAction::AckSent,
                      datagram.destinationId, datagram.data.length());
        }

        servicePackets.erase(datagramIt);
        return ResultType::Continue;
    }

    if (iface != 0 || rayPackets.empty()) return ResultType::Continue;

    /* packet to send */
    RayPacket& packet = rayPackets.front();

    /* peer to send the packet to */
    sendUdpPacket(packet.destination, packet.data(), 0);

    /* do the necessary logging */
    if (packet.retransmission) {
        workerStats.recordResentRays(packet.targetTreelet, packet.rayCount);
    } else {
        workerStats.recordSentRays(packet.targetTreelet, packet.rayCount);
    }

    for (auto& rayPtr : packet.trackedRays) {
        logRayAction(*rayPtr, RayAction::Sent, packet.destinationId);
        rayPtr->tick++;
    }

    if (trackPackets && packet.tracked) {
        logPacket(packet.sequenceNumber, packet.attempt, PacketAction::Sent,
                  packet.destinationId, packet.data().length(),
                  packet.rayCount);
    }

    if (packet.reliable) {
        outstandingRayPackets.emplace_back(packet_clock::now() + PACKET_TIMEOUT,
                                           move(packet));
    }

    rayPackets.pop_front();
    return ResultType::Continue;
}

ResultType LambdaWorker::handleUdpReceive(const size_t iface) {
    RECORD_INTERVAL("receiveUDP");

    auto datagram = udpConnection[iface].socket().recvfrom();
    auto& data = datagram.second;
    udpConnection[iface].bytes_received += data.length();

    messageParser.parse(data);
    auto& messages = messageParser.completed_messages();

    auto it = messages.end();
    while (it != messages.begin()) {
        it--;

        if (it->is_read()) break;
        it->set_read();

        if (it->reliable()) {
            const auto seqNo = it->sequence_number();
            toBeAcked[datagram.first].emplace_back(seqNo, it->tracked(),
                                                   it->attempt());

            auto& received = receivedPacketSeqNos[datagram.first];

            if (it->tracked()) {
                logPacket(it->sequence_number(), it->attempt(),
                          PacketAction::Received, it->sender_id(),
                          it->total_length());
            }

            if (received.contains(seqNo)) {
                it = messages.erase(it);
                continue;
            } else {
                received.insert(seqNo);
            }
        }

        if (it->opcode() == OpCode::Ack) {
            Chunk chunk(it->payload());
            auto& thisReceivedAcks = receivedAcks[datagram.first];

            if (it->tracked()) {
                logPacket(it->sequence_number(), it->attempt(),
                          PacketAction::AckReceived, it->sender_id(),
                          it->total_length(), 0u);
            }

            while (chunk.size()) {
                const auto seqNo = chunk.be64();
                thisReceivedAcks.insert(seqNo);
                chunk = chunk(8);

                const bool tracked = chunk.octet();
                chunk = chunk(1);

                const uint16_t attempt = chunk.be16();
                chunk = chunk(2);

                if (tracked) {
                    logPacket(seqNo, attempt, PacketAction::Acked,
                              it->sender_id(), 0u);
                }
            }

            it = messages.erase(it);
        }
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleWorkerStats() {
    RECORD_INTERVAL("handleWorkerStats");
    workerStatsTimer.reset();

    auto& qStats = workerStats.queueStats;
    qStats.ray = rayQueue.size();
    qStats.finished = finishedQueue.size();
    qStats.pending = pendingQueueSize;
    qStats.out = outQueueSize;
    qStats.connecting =
        count_if(peers.begin(), peers.end(), [](const auto& peer) {
            return peer.second.state == Worker::State::Connecting;
        });
    qStats.connected = peers.size() - qStats.connecting;
    qStats.outstandingUdp = outstandingRayPackets.size();
    qStats.queuedUdp = rayPackets.size();

    auto proto = to_protobuf(workerStats);

    proto.set_timestamp_us(
        duration_cast<microseconds>(now() - workerStats.startTime).count());

    Message message{*workerId, OpCode::WorkerStats,
                    protoutil::to_string(proto)};
    coordinatorConnection->enqueue_write(message.str());
    workerStats.reset();
    return ResultType::Continue;
}

ResultType LambdaWorker::handleDiagnostics() {
    RECORD_INTERVAL("handleDiagnostics");
    workerDiagnosticsTimer.reset();

    workerDiagnostics.bytesSent =
        udpConnection[0].bytes_sent - lastDiagnostics.bytesSent;

    workerDiagnostics.bytesReceived =
        udpConnection[0].bytes_received - lastDiagnostics.bytesReceived;

    workerDiagnostics.outstandingUdp = rayPackets.size();
    lastDiagnostics.bytesSent = udpConnection[0].bytes_sent;
    lastDiagnostics.bytesReceived = udpConnection[0].bytes_received;

    const auto timestamp =
        duration_cast<microseconds>(now() - workerDiagnostics.startTime)
            .count();

    TLOG(DIAG) << timestamp << " "
               << protoutil::to_json(to_protobuf(workerDiagnostics));

    workerDiagnostics.reset();

    return ResultType::Continue;
}

void LambdaWorker::generateRays(const Bounds2i& bounds) {
    const Bounds2i sampleBounds = camera->film->GetSampleBounds();
    const Vector2i sampleExtent = sampleBounds.Diagonal();
    const auto samplesPerPixel = sampler->samplesPerPixel;
    const Float rayScale = 1 / sqrt((Float)samplesPerPixel);

    /* for ray tracking */
    bernoulli_distribution bd{config.rayActionsLogRate};

    for (size_t sample = 0; sample < sampler->samplesPerPixel; sample++) {
        for (const Point2i pixel : bounds) {
            sampler->StartPixel(pixel);
            if (!InsideExclusive(pixel, sampleBounds)) continue;
            sampler->SetSampleNumber(sample);

            CameraSample cameraSample = sampler->GetCameraSample(pixel);

            RayStatePtr statePtr = make_unique<RayState>();
            RayState& state = *statePtr;

            state.trackRay = trackRays ? bd(randEngine) : false;
            state.sample.id =
                (pixel.x + pixel.y * sampleExtent.x) * config.samplesPerPixel +
                sample;
            state.sample.num = sample;
            state.sample.pixel = pixel;
            state.sample.pFilm = cameraSample.pFilm;
            state.sample.weight =
                camera->GenerateRayDifferential(cameraSample, &state.ray);
            state.ray.ScaleDifferentials(rayScale);
            state.remainingBounces = maxDepth;
            state.StartTrace();

            logRayAction(state, RayAction::Generated);
            workerStats.recordDemandedRay(state);

            const auto nextTreelet = state.CurrentTreelet();

            if (treeletIds.count(nextTreelet)) {
                pushRayQueue(move(statePtr));
            } else {
                if (treeletToWorker.count(nextTreelet)) {
                    workerStats.recordSendingRay(state);
                    outQueue[nextTreelet].push_back(move(statePtr));
                    outQueueSize++;
                } else {
                    workerStats.recordPendingRay(state);
                    neededTreelets.insert(nextTreelet);
                    pendingQueue[nextTreelet].push_back(move(statePtr));
                    pendingQueueSize++;
                }
            }
        }
    }
}

void LambdaWorker::getObjects(const protobuf::GetObjects& objects) {
    vector<storage::GetRequest> requests;
    for (const protobuf::ObjectKey& objectKey : objects.object_ids()) {
        const ObjectKey id = from_protobuf(objectKey);
        if (id.type == ObjectType::TriangleMesh) {
            /* triangle meshes are packed into treelets, so ignore */
            continue;
        }
        if (id.type == ObjectType::Treelet) {
            treeletIds.insert(id.id);
        }
        const string filePath = id.to_string();
        requests.emplace_back(filePath, filePath);
    }
    storageBackend->get(requests);
}

void LambdaWorker::pushRayQueue(RayStatePtr&& state) {
    workerStats.recordWaitingRay(*state);
    rayQueue.push_back(move(state));
}

RayStatePtr LambdaWorker::popRayQueue() {
    RayStatePtr state = move(rayQueue.front());
    rayQueue.pop_front();

    workerStats.recordProcessedRay(*state);

    return state;
}

bool LambdaWorker::processMessage(const Message& message) {
    /* cerr << "[msg:" << Message::OPCODE_NAMES[to_underlying(message.opcode())]
         << "]\n"; */

    auto handleConnectTo = [this](const protobuf::ConnectTo& proto) {
        if (peers.count(proto.worker_id()) == 0 &&
            proto.worker_id() != *workerId) {
            auto& peer =
                peers.emplace(proto.worker_id(), Worker{proto.worker_id()})
                    .first->second;

            for (size_t i = 0; i < min(2, proto.address_size()); i++) {
                auto addrPair = Address::decompose(proto.address(i));
                peer.address[i] = {addrPair.first, addrPair.second};
                addressToWorker.emplace(peer.address[i], proto.worker_id());
            }
        }
    };

    switch (message.opcode()) {
    case OpCode::Hey: {
        protobuf::Hey proto;
        protoutil::from_string(message.payload(), proto);
        workerId.reset(proto.worker_id());
        jobId.reset(proto.job_id());

        logPrefix = "logs/" + (*jobId) + "/";
        outputName = to_string(*workerId) + ".rays";

        cerr << "worker-id=" << *workerId << endl;

        /* send connection request */
        auto& peer = peers.emplace(0, Worker{0}).first->second;
        peer.address[0] = peer.address[1] = coordinatorAddr;

        for (size_t i = 0; i < 2; i++) {
            Message message = createConnectionRequest(peers.at(0), i);
            servicePackets.emplace_front(coordinatorAddr, 0, message.str());
            servicePackets.front().iface = i;
        }

        break;
    }

    case OpCode::Ping: {
        /* Message pong{OpCode::Pong, ""};
        coordinatorConnection->enqueue_write(pong.str()); */
        break;
    }

    case OpCode::GetObjects: {
        protobuf::GetObjects proto;
        protoutil::from_string(message.payload(), proto);
        getObjects(proto);
        initializeScene();
        break;
    }

    case OpCode::GenerateRays: {
        RECORD_INTERVAL("generateRays");
        protobuf::GenerateRays proto;
        protoutil::from_string(message.payload(), proto);
        generateRays(from_protobuf(proto.crop_window()));
        break;
    }

    case OpCode::ConnectTo: {
        protobuf::ConnectTo proto;
        protoutil::from_string(message.payload(), proto);
        handleConnectTo(proto);
        break;
    }

    case OpCode::MultipleConnect: {
        protobuf::ConnectTo proto;
        protobuf::RecordReader reader{istringstream{message.payload()}};

        while (!reader.eof()) {
            reader.read(&proto);
            handleConnectTo(proto);
        }

        break;
    }

    case OpCode::ConnectionRequest: {
        protobuf::ConnectRequest proto;
        protoutil::from_string(message.payload(), proto);

        const auto otherWorkerId = proto.worker_id();
        if (peers.count(otherWorkerId) == 0) {
            /* we haven't heard about this peer from the master, let's process
             * it later */
            return false;
        }

        auto& peer = peers.at(otherWorkerId);
        auto message = createConnectionResponse(peer, proto.address_no());
        servicePackets.emplace_front(peer.address[proto.address_no()],
                                     otherWorkerId, message.str());
        servicePackets.front().iface = proto.address_no();
        break;
    }

    case OpCode::ConnectionResponse: {
        protobuf::ConnectResponse proto;
        protoutil::from_string(message.payload(), proto);

        const auto otherWorkerId = proto.worker_id();
        if (peers.count(otherWorkerId) == 0) {
            /* we don't know about this worker */
            return true;
        }

        auto& peer = peers.at(otherWorkerId);
        peer.seed = proto.my_seed();

        if (peer.state != Worker::State::Connected &&
            proto.your_seed() == mySeed) {
            peer.connected[proto.address_no()] = true;
            peer.state =
                (accumulate(peer.connected, peer.connected + 2, 0) == 2)
                    ? Worker::State::Connected
                    : Worker::State::Connecting;

            if (peer.state != Worker::State::Connected) {
                break;
            }

            peer.nextKeepAlive = packet_clock::now() + KEEP_ALIVE_INTERVAL;

            for (const auto treeletId : proto.treelet_ids()) {
                peer.treelets.insert(treeletId);
                treeletToWorker[treeletId].push_back(otherWorkerId);
                neededTreelets.erase(treeletId);
                requestedTreelets.erase(treeletId);

                if (pendingQueue.count(treeletId)) {
                    auto& treeletPending = pendingQueue[treeletId];
                    auto& treeletOut = outQueue[treeletId];

                    outQueueSize += treeletPending.size();
                    pendingQueueSize -= treeletPending.size();

                    while (!treeletPending.empty()) {
                        auto& front = treeletPending.front();
                        workerStats.recordSendingRay(*front);
                        treeletOut.push_back(move(front));
                        treeletPending.pop_front();
                    }
                }
            }
        }

        break;
    }

    case OpCode::SendRays: {
        protobuf::RecordReader reader{istringstream{message.payload()}};

        while (!reader.eof()) {
            string rayStr;
            if (reader.read(&rayStr)) {
                RayStatePtr ray = RayState::deserialize(rayStr);
                ray->hop++;
                workerStats.recordReceivedRay(*ray);
                logRayAction(*ray, RayAction::Received, message.sender_id());
                ray->tick = 0;
                pushRayQueue(move(ray));
            }
        }

        break;
    }

    case OpCode::Bye:
        terminate();
        break;

    case OpCode::StartBenchmark: {
        Chunk c{message.payload()};
        const uint32_t destination = c.be32();
        const uint32_t duration = c(4).be32();
        const uint32_t rate = c(8).be32();
        const uint32_t addressNo = c(12).be32();
        initBenchmark(duration, destination, rate, addressNo);
        break;
    }

    default:
        throw runtime_error("unhandled message opcode");
    }

    return true;
}

// Minimum, where negative numbers are regarded as infinitely positive.
int min_neg_infinity(const int a, const int b) {
    if (a < 0) return b;
    if (b < 0) return a;
    return min(a, b);
}

void LambdaWorker::run() {
    while (!terminated) {
        // timeouts treat -1 as positive infinity
        int min_timeout_ms = -1;

        for (size_t i = 0; i < 2; i++) {
            // If this connection is not within pace, it requests a timeout when
            // it would be, so that we can re-poll and schedule it.
            const int64_t ahead_us = udpConnection[i].micros_ahead_of_pace();
            int64_t ahead_ms = ahead_us / 1000;
            if (ahead_us != 0 && ahead_ms == 0) ahead_ms = 1;

            const int timeout_ms =
                udpConnection[i].within_pace() ? -1 : ahead_ms;
            min_timeout_ms = min_neg_infinity(min_timeout_ms, timeout_ms);
        }

        auto res = loop.loop_once(min_timeout_ms).result;
        if (res != PollerResult::Success && res != PollerResult::Timeout) break;
    }
}

void LambdaWorker::loadCamera() {
    auto reader = manager.GetReader(ObjectType::Camera);
    protobuf::Camera proto_camera;
    reader->read(&proto_camera);
    camera = camera::from_protobuf(proto_camera, transformCache);
    filmTile = camera->film->GetFilmTile(camera->film->GetSampleBounds());
}

void LambdaWorker::loadSampler() {
    auto reader = manager.GetReader(ObjectType::Sampler);
    protobuf::Sampler proto_sampler;
    reader->read(&proto_sampler);
    sampler = sampler::from_protobuf(proto_sampler, config.samplesPerPixel);

    /* if (workerId.initialized()) {
        sampler = sampler->Clone(*workerId);
    } */
}

void LambdaWorker::loadLights() {
    auto reader = manager.GetReader(ObjectType::Lights);
    while (!reader->eof()) {
        protobuf::Light proto_light;
        reader->read(&proto_light);
        lights.push_back(move(light::from_protobuf(proto_light)));
    }
}

void LambdaWorker::loadFakeScene() {
    auto reader = manager.GetReader(ObjectType::Scene);
    protobuf::Scene proto_scene;
    reader->read(&proto_scene);
    fakeScene = make_unique<Scene>(from_protobuf(proto_scene));
}

void LambdaWorker::initializeScene() {
    if (initialized) return;

    loadCamera();
    loadSampler();
    loadLights();
    loadFakeScene();

    for (auto& light : lights) {
        light->Preprocess(*fakeScene);
    }

    initialized = true;
}

void LambdaWorker::uploadLogs() {
    if (!workerId.initialized()) return;

    benchmarkData.stats.merge(benchmarkData.checkpoint);

    TLOG(BENCH) << "start "
                << duration_cast<milliseconds>(
                       benchmarkData.start.time_since_epoch())
                       .count();

    TLOG(BENCH) << "end "
                << duration_cast<milliseconds>(
                       benchmarkData.end.time_since_epoch())
                       .count();

    for (const auto& item : benchmarkData.checkpoints) {
        TLOG(BENCH) << "checkpoint "
                    << duration_cast<milliseconds>(
                           item.timestamp.time_since_epoch())
                           .count()
                    << " " << item.bytesSent << " " << item.bytesReceived << " "
                    << item.packetsSent << " " << item.packetsReceived;
    }

    TLOG(BENCH) << "stats " << benchmarkData.stats.bytesSent << " "
                << benchmarkData.stats.bytesReceived << " "
                << benchmarkData.stats.packetsSent << " "
                << benchmarkData.stats.packetsReceived;

    google::FlushLogFiles(google::INFO);

    vector<storage::PutRequest> putLogsRequest = {
        {infoLogName, logPrefix + to_string(*workerId) + ".INFO"}};

    storageBackend->put(putLogsRequest);
}

void usage(const char* argv0, int exitCode) {
    cerr << "Usage: " << argv0 << " [OPTIONS]" << endl
         << endl
         << "Options:" << endl
         << "  -i --ip IPSTRING           ip of coordinator" << endl
         << "  -p --port PORT             port of coordinator" << endl
         << "  -s --storage-backend NAME  storage backend URI" << endl
         << "  -R --reliable-udp          send ray packets reliably" << endl
         << "  -M --max-udp-rate RATE     maximum UDP rate (Mbps)" << endl
         << "  -S --samples N             number of samples per pixel" << endl
         << "  -L --log-rays RATE         log ray actions" << endl
         << "  -P --log-packets RATE      log packets" << endl
         << "  -f --finished-ray ACTION   what to do with finished rays" << endl
         << "                             * 0: discard (default)" << endl
         << "                             * 1: send" << endl
         << "                             * 2: upload" << endl
         << "  -h --help                  show help information" << endl;
}

int main(int argc, char* argv[]) {
    int exit_status = EXIT_SUCCESS;

    uint16_t listenPort = 50000;
    string publicIp;
    string storageUri;

    bool sendReliably = false;
    uint64_t maxUdpRate = 80;
    int samplesPerPixel = 0;
    FinishedRayAction finishedRayAction = FinishedRayAction::Discard;
    float rayActionsLogRate = 0.0;
    float packetsLogRate = 0.0;

    struct option long_options[] = {
        {"port", required_argument, nullptr, 'p'},
        {"ip", required_argument, nullptr, 'i'},
        {"storage-backend", required_argument, nullptr, 's'},
        {"reliable-udp", no_argument, nullptr, 'R'},
        {"samples", required_argument, nullptr, 'S'},
        {"log-rays", required_argument, nullptr, 'L'},
        {"log-packets", required_argument, nullptr, 'P'},
        {"finished-ray", required_argument, nullptr, 'f'},
        {"max-udp-rate", required_argument, nullptr, 'M'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    while (true) {
        const int opt = getopt_long(argc, argv, "p:i:s:S:f:L:P:M:hR",
                                    long_options, nullptr);

        if (opt == -1) break;

        // clang-format off
        switch (opt) {
        case 'p': listenPort = stoi(optarg); break;
        case 'i': publicIp = optarg; break;
        case 's': storageUri = optarg; break;
        case 'R': sendReliably = true; break;
        case 'M': maxUdpRate = stoull(optarg); break;
        case 'S': samplesPerPixel = stoi(optarg); break;
        case 'L': rayActionsLogRate = stof(optarg); break;
        case 'P': packetsLogRate = stof(optarg); break;
        case 'f': finishedRayAction = (FinishedRayAction)stoi(optarg); break;
        case 'h': usage(argv[0], EXIT_SUCCESS); break;
        default: usage(argv[0], EXIT_FAILURE);
        }
        // clang-format on
    }

    if (listenPort == 0 || rayActionsLogRate < 0 || rayActionsLogRate > 1.0 ||
        packetsLogRate < 0 || packetsLogRate > 1.0 || publicIp.empty() ||
        storageUri.empty() || maxUdpRate == 0) {
        usage(argv[0], EXIT_FAILURE);
    }

    unique_ptr<LambdaWorker> worker;
    WorkerConfiguration config{sendReliably,      maxUdpRate,
                               samplesPerPixel,   finishedRayAction,
                               rayActionsLogRate, packetsLogRate};

    try {
        worker =
            make_unique<LambdaWorker>(publicIp, listenPort, storageUri, config);
        worker->run();
    } catch (const exception& e) {
        cerr << argv[0] << ": " << e.what() << endl;
        exit_status = EXIT_FAILURE;
    }

    if (worker) {
        worker->uploadLogs();
    }

    return exit_status;
}
