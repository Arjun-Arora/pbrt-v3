#ifndef PBRT_MESSAGES_UTILS_H
#define PBRT_MESSAGES_UTILS_H

#include "geometry.h"
#include "transform.h"
#include "pbrt.pb.h"

namespace pbrt {

    protobuf::Point3f to_protobuf(const Point3f & point);
    protobuf::Bounds3f to_protobuf(const Bounds3f & bounds);
    protobuf::Matrix to_protobuf(const Matrix4x4 & matrix);
    protobuf::AnimatedTransform to_protobuf(const AnimatedTransform & transform);

    Point3f from_protobuf(const protobuf::Point3f & point);
    Bounds3f from_protobuf(const protobuf::Bounds3f & bounds);
    Matrix4x4 from_protobuf(const protobuf::Matrix & matrix);

}

#endif /* PBRT_MESSAGES_UTILS_H */