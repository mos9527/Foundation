#include "JoltPhysicsWorld.hpp"
#include "JoltCommon.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <Core/Container.hpp>
#include <Core/Logging.hpp>

JPH_SUPPRESS_WARNINGS

namespace Foundation::Examples
{
    namespace
    {
        namespace Layers
        {
            static constexpr JPH::ObjectLayer NON_MOVING = 0;
            static constexpr JPH::ObjectLayer MOVING = 1;
            static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
        }

        namespace BroadPhaseLayers
        {
            static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
            static constexpr JPH::BroadPhaseLayer MOVING(1);
            static constexpr JPH::uint NUM_LAYERS(2);
        }

        class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
        {
        public:
            bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
            {
                switch (inObject1)
                {
                case Layers::NON_MOVING:
                    return inObject2 == Layers::MOVING;
                case Layers::MOVING:
                    return true;
                default:
                    return false;
                }
            }
        };

        class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
        {
        public:
            BPLayerInterfaceImpl()
            {
                mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
                mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
            }

            JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
            {
                return mObjectToBroadPhase[inLayer];
            }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            char const* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
            {
                switch ((JPH::BroadPhaseLayer::Type)inLayer)
                {
                case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
                    return "NON_MOVING";
                case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
                    return "MOVING";
                default:
                    return "INVALID";
                }
            }
#endif
        private:
            JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
        };

        class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
            {
                switch (inLayer1)
                {
                case Layers::NON_MOVING:
                    return inLayer2 == BroadPhaseLayers::MOVING;
                case Layers::MOVING:
                    return true;
                default:
                    return false;
                }
            }
        };

        class QueryLayerFilter final : public JPH::ObjectLayerFilter
        {
        public:
            explicit QueryLayerFilter(uint32_t mask) : mMask(mask) {}
            bool ShouldCollide(JPH::ObjectLayer layer) const override
            {
                return layer < 32 && (mMask & (1u << layer)) != 0;
            }

        private:
            uint32_t mMask;
        };

        JPH::Vec3 ToJolt(float3 const& v) { return JPH::Vec3(v.x, v.y, v.z); }
        JPH::RVec3 ToJoltR(float3 const& v) { return JPH::RVec3(v.x, v.y, v.z); }
        JPH::Quat ToJolt(quat const& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
        float3 FromJolt(JPH::Vec3Arg v) { return float3(v.GetX(), v.GetY(), v.GetZ()); }
        float3 FromJoltR(JPH::RVec3Arg v) { return float3(v.GetX(), v.GetY(), v.GetZ()); }
        quat FromJolt(JPH::QuatArg q) { return quat(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }

        JPH::EMotionType ToJolt(PhysicsMotionType motion)
        {
            switch (motion)
            {
            case PhysicsMotionType::Static:
                return JPH::EMotionType::Static;
            case PhysicsMotionType::Kinematic:
                return JPH::EMotionType::Kinematic;
            case PhysicsMotionType::Dynamic:
                return JPH::EMotionType::Dynamic;
            }
            return JPH::EMotionType::Dynamic;
        }

        JPH::EActivation ToJolt(PhysicsActivation activation)
        {
            return activation == PhysicsActivation::Activate ? JPH::EActivation::Activate
                                                             : JPH::EActivation::DontActivate;
        }
    } // namespace

    struct JoltPhysicsWorld::Impl
    {
        struct ShapeSlot
        {
            uint32_t generation{1};
            uint32_t refCount{0};
            bool live{false};
            JPH::ShapeRefC shape;
        };

        struct BodySlot
        {
            uint32_t generation{1};
            bool live{false};
            PhysicsShapeHandle shape{};
            JPH::BodyID bodyId{};
        };

        Core::Allocator* allocator{GLOBAL_ALLOC};
        JPH::TempAllocatorImpl tempAllocator;
        FoundationJoltJobSystem jobSystem;
        BPLayerInterfaceImpl broadPhaseLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
        ObjectLayerPairFilterImpl objectLayerPairFilter;
        JPH::PhysicsSystem physicsSystem;
        Core::Vector<ShapeSlot> shapes;
        Core::Vector<BodySlot> bodies;
        Core::Vector<uint32_t> freeShapes;
        Core::Vector<uint32_t> freeBodies;

        Impl(Core::JobSystem* jobs, PhysicsWorldDesc const& desc) :
            tempAllocator(desc.tempAllocatorBytes),
            jobSystem(jobs, desc.maxJobs, desc.maxBarriers),
            shapes(allocator),
            bodies(allocator),
            freeShapes(allocator),
            freeBodies(allocator)
        {
            physicsSystem.Init(desc.maxBodies, desc.numBodyMutexes, desc.maxBodyPairs, desc.maxContactConstraints,
                               broadPhaseLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter);
            physicsSystem.SetGravity(ToJolt(desc.gravity));
            shapes.reserve(desc.maxBodies);
            bodies.reserve(desc.maxBodies);
            freeShapes.reserve(desc.maxBodies);
            freeBodies.reserve(desc.maxBodies);
        }

        [[nodiscard]] ShapeSlot* ResolveShape(PhysicsShapeHandle handle)
        {
            if (!handle.IsValid() || handle.index >= shapes.size())
                return nullptr;
            ShapeSlot& slot = shapes[handle.index];
            if (!slot.live || slot.generation != handle.generation)
                return nullptr;
            return &slot;
        }

        [[nodiscard]] ShapeSlot const* ResolveShape(PhysicsShapeHandle handle) const
        {
            return const_cast<Impl*>(this)->ResolveShape(handle);
        }

        [[nodiscard]] BodySlot* ResolveBody(PhysicsBodyHandle handle)
        {
            if (!handle.IsValid() || handle.index >= bodies.size())
                return nullptr;
            BodySlot& slot = bodies[handle.index];
            if (!slot.live || slot.generation != handle.generation)
                return nullptr;
            return &slot;
        }

        [[nodiscard]] BodySlot const* ResolveBody(PhysicsBodyHandle handle) const
        {
            return const_cast<Impl*>(this)->ResolveBody(handle);
        }

        void DestroyBodySlot(BodySlot& slot)
        {
            JPH::BodyInterface& bodyInterface = physicsSystem.GetBodyInterface();
            bodyInterface.RemoveBody(slot.bodyId);
            bodyInterface.DestroyBody(slot.bodyId);
            if (ShapeSlot* shape = ResolveShape(slot.shape))
                --shape->refCount;
            slot.live = false;
            slot.bodyId = {};
            slot.shape = {};
            ++slot.generation;
        }
    };

    JoltPhysicsWorld::JoltPhysicsWorld(Core::JobSystem* jobs, PhysicsWorldDesc const& desc) : mDesc(desc)
    {
        CHECK(jobs != nullptr);
        CHECK(desc.maxBodies > 0);
        CHECK(desc.maxJobs > 0);
        CHECK(desc.maxBarriers > 0);
        CHECK(desc.tempAllocatorBytes > 0);

        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        mImpl = new Impl(jobs, desc);
    }

    JoltPhysicsWorld::~JoltPhysicsWorld()
    {
        if (!mImpl)
            return;

        JPH::BodyInterface& bodyInterface = mImpl->physicsSystem.GetBodyInterface();
        (void)bodyInterface;
        for (Impl::BodySlot& slot : mImpl->bodies)
        {
            if (slot.live)
                mImpl->DestroyBodySlot(slot);
        }
        mImpl->bodies.clear();
        mImpl->shapes.clear();

        delete mImpl;
        mImpl = nullptr;

        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    PhysicsStatus JoltPhysicsWorld::CreateShape(PhysicsShapeDesc const& desc, PhysicsShapeHandle& outHandle)
    {
        outHandle = {};
        JPH::Shape::ShapeResult result;
        switch (desc.type)
        {
        case PhysicsShapeType::Box:
        {
            if (!(desc.halfExtent.x > 0.0f && desc.halfExtent.y > 0.0f && desc.halfExtent.z > 0.0f))
                return PhysicsStatus::InvalidArgument;
            JPH::BoxShapeSettings settings(ToJolt(desc.halfExtent));
            settings.SetEmbedded();
            result = settings.Create();
            break;
        }
        case PhysicsShapeType::Sphere:
        {
            if (!(desc.radius > 0.0f))
                return PhysicsStatus::InvalidArgument;
            JPH::SphereShapeSettings settings(desc.radius);
            settings.SetEmbedded();
            result = settings.Create();
            break;
        }
        case PhysicsShapeType::Capsule:
        {
            if (!(desc.radius > 0.0f && desc.halfHeight > 0.0f))
                return PhysicsStatus::InvalidArgument;
            JPH::CapsuleShapeSettings settings(desc.halfHeight, desc.radius);
            settings.SetEmbedded();
            result = settings.Create();
            break;
        }
        default:
            return PhysicsStatus::InvalidArgument;
        }

        if (result.HasError())
            return PhysicsStatus::JoltFailure;

        uint32_t index;
        if (!mImpl->freeShapes.empty())
        {
            index = mImpl->freeShapes.back();
            mImpl->freeShapes.pop_back();
        }
        else
        {
            index = static_cast<uint32_t>(mImpl->shapes.size());
            mImpl->shapes.push_back({});
        }

        Impl::ShapeSlot& slot = mImpl->shapes[index];
        slot.live = true;
        slot.refCount = 0;
        slot.shape = result.Get();
        outHandle = PhysicsShapeHandle{.index = index, .generation = slot.generation};
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::DestroyShape(PhysicsShapeHandle handle)
    {
        Impl::ShapeSlot* slot = mImpl->ResolveShape(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        if (slot->refCount != 0)
            return PhysicsStatus::InvalidArgument;

        slot->live = false;
        slot->shape = nullptr;
        ++slot->generation;
        mImpl->freeShapes.push_back(handle.index);
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::CreateBody(PhysicsBodyDesc const& desc, PhysicsBodyHandle& outHandle)
    {
        outHandle = {};
        if (mBodyCount >= mDesc.maxBodies)
            return PhysicsStatus::CapacityExceeded;
        if (desc.layer != kPhysicsLayerNonMoving && desc.layer != kPhysicsLayerMoving)
            return PhysicsStatus::InvalidArgument;

        Impl::ShapeSlot* shape = mImpl->ResolveShape(desc.shape);
        if (!shape)
            return PhysicsStatus::InvalidHandle;

        JPH::BodyCreationSettings settings(shape->shape, ToJoltR(desc.pose.position), ToJolt(desc.pose.rotation),
                                           ToJolt(desc.motion), static_cast<JPH::ObjectLayer>(desc.layer));
        JPH::BodyInterface& bodyInterface = mImpl->physicsSystem.GetBodyInterface();
        JPH::BodyID bodyId = bodyInterface.CreateAndAddBody(settings, ToJolt(desc.activation));
        if (bodyId.IsInvalid())
            return PhysicsStatus::JoltFailure;

        if (desc.linearVelocity != float3(0.0f) || desc.angularVelocity != float3(0.0f))
        {
            bodyInterface.SetLinearAndAngularVelocity(bodyId, ToJolt(desc.linearVelocity),
                                                      ToJolt(desc.angularVelocity));
        }

        uint32_t index;
        if (!mImpl->freeBodies.empty())
        {
            index = mImpl->freeBodies.back();
            mImpl->freeBodies.pop_back();
        }
        else
        {
            index = static_cast<uint32_t>(mImpl->bodies.size());
            mImpl->bodies.push_back({});
        }

        Impl::BodySlot& slot = mImpl->bodies[index];
        slot.live = true;
        slot.shape = desc.shape;
        slot.bodyId = bodyId;
        bodyInterface.SetUserData(bodyId, (static_cast<uint64_t>(slot.generation) << 32u) | index);
        ++shape->refCount;
        ++mBodyCount;
        outHandle = PhysicsBodyHandle{.index = index, .generation = slot.generation};
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::DestroyBody(PhysicsBodyHandle handle)
    {
        Impl::BodySlot* slot = mImpl->ResolveBody(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;

        mImpl->DestroyBodySlot(*slot);
        mImpl->freeBodies.push_back(handle.index);
        --mBodyCount;
        return PhysicsStatus::Ok;
    }

    bool JoltPhysicsWorld::IsBodyValid(PhysicsBodyHandle handle) const
    {
        return mImpl->ResolveBody(handle) != nullptr;
    }

    PhysicsStatus JoltPhysicsWorld::SetBodyPose(PhysicsBodyHandle handle, PhysicsBodyPose const& pose,
                                                PhysicsActivation activation)
    {
        Impl::BodySlot* slot = mImpl->ResolveBody(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        mImpl->physicsSystem.GetBodyInterface().SetPositionAndRotation(slot->bodyId, ToJoltR(pose.position),
                                                                       ToJolt(pose.rotation), ToJolt(activation));
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::GetBodyPose(PhysicsBodyHandle handle, PhysicsBodyPose& outPose) const
    {
        Impl::BodySlot const* slot = mImpl->ResolveBody(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;

        JPH::BodyInterface const& bodyInterface = mImpl->physicsSystem.GetBodyInterface();
        outPose.position = FromJoltR(bodyInterface.GetPosition(slot->bodyId));
        outPose.rotation = FromJolt(bodyInterface.GetRotation(slot->bodyId));
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::SetLinearVelocity(PhysicsBodyHandle handle, float3 const& velocity)
    {
        Impl::BodySlot* slot = mImpl->ResolveBody(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        mImpl->physicsSystem.GetBodyInterface().SetLinearVelocity(slot->bodyId, ToJolt(velocity));
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::GetLinearVelocity(PhysicsBodyHandle handle, float3& outVelocity) const
    {
        Impl::BodySlot const* slot = mImpl->ResolveBody(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        outVelocity = FromJolt(mImpl->physicsSystem.GetBodyInterface().GetLinearVelocity(slot->bodyId));
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::AddForce(PhysicsBodyHandle handle, float3 const& force)
    {
        Impl::BodySlot* slot = mImpl->ResolveBody(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        mImpl->physicsSystem.GetBodyInterface().AddForce(slot->bodyId, ToJolt(force));
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::AddImpulse(PhysicsBodyHandle handle, float3 const& impulse)
    {
        Impl::BodySlot* slot = mImpl->ResolveBody(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        mImpl->physicsSystem.GetBodyInterface().AddImpulse(slot->bodyId, ToJolt(impulse));
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::RayCast(PhysicsRayDesc const& ray, PhysicsRayHit& outHit) const
    {
        outHit = {};
        float const directionLength = length(ray.direction);
        if (!(directionLength > 1e-6f) || !(ray.maxDistance > 0.0f))
            return PhysicsStatus::InvalidArgument;

        JPH::RRayCast const cast(ToJoltR(ray.origin), ToJolt(ray.direction / directionLength * ray.maxDistance));
        JPH::RayCastResult hit;
        QueryLayerFilter const layerFilter(ray.layerMask);
        if (!mImpl->physicsSystem.GetNarrowPhaseQuery().CastRay(cast, hit, {}, layerFilter))
            return PhysicsStatus::NoHit;

        JPH::BodyInterface const& bodyInterface = mImpl->physicsSystem.GetBodyInterface();
        uint64_t const userData = bodyInterface.GetUserData(hit.mBodyID);
        PhysicsBodyHandle const body{
            .index = static_cast<uint32_t>(userData),
            .generation = static_cast<uint32_t>(userData >> 32u),
        };
        Impl::BodySlot const* slot = mImpl->ResolveBody(body);
        if (!slot || slot->bodyId != hit.mBodyID)
            return PhysicsStatus::InvalidHandle;

        JPH::RVec3 const position = cast.GetPointOnRay(hit.mFraction);
        JPH::Vec3 const normal =
            bodyInterface.GetTransformedShape(hit.mBodyID).GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, position);
        outHit = {
            .body = body,
            .position = FromJoltR(position),
            .normal = FromJolt(normal),
            .distance = hit.mFraction * ray.maxDistance,
        };
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltPhysicsWorld::Step(float deltaSeconds, int collisionSteps)
    {
        if (!(deltaSeconds >= 0.0f) || collisionSteps < 1)
            return PhysicsStatus::InvalidArgument;
        mImpl->physicsSystem.Update(deltaSeconds, collisionSteps, &mImpl->tempAllocator, &mImpl->jobSystem);
        return PhysicsStatus::Ok;
    }

    void JoltPhysicsWorld::OptimizeBroadPhase() { mImpl->physicsSystem.OptimizeBroadPhase(); }
} // namespace Foundation::Examples
