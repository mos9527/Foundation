#pragma once
#include <Core/JobSystem.hpp>
#include <Math/Math.hpp>
#include <cstdint>

namespace Foundation::Examples
{
    using Math::float3;
    using Math::quat;

    enum class PhysicsStatus : uint32_t
    {
        Ok = 0,
        InvalidArgument = 1,
        InvalidHandle = 2,
        CapacityExceeded = 3,
        JoltFailure = 4,
        NoHit = 5,
    };

    enum class PhysicsShapeType : uint32_t
    {
        Box = 0,
        Sphere = 1,
        Capsule = 2,
    };

    enum class PhysicsMotionType : uint32_t
    {
        Static = 0,
        Kinematic = 1,
        Dynamic = 2,
    };

    enum class PhysicsActivation : uint32_t
    {
        Activate = 0,
        DontActivate = 1,
    };

    inline constexpr uint32_t kPhysicsLayerNonMoving = 0u;
    inline constexpr uint32_t kPhysicsLayerMoving = 1u;

    struct PhysicsShapeHandle
    {
        uint32_t index{~0u};
        uint32_t generation{0};
        [[nodiscard]] bool IsValid() const { return index != ~0u; }
        [[nodiscard]] bool operator==(PhysicsShapeHandle const&) const = default;
    };

    struct PhysicsBodyHandle
    {
        uint32_t index{~0u};
        uint32_t generation{0};
        [[nodiscard]] bool IsValid() const { return index != ~0u; }
        [[nodiscard]] bool operator==(PhysicsBodyHandle const&) const = default;
    };

    struct PhysicsWorldDesc
    {
        uint32_t maxBodies{1024};
        uint32_t numBodyMutexes{0};
        uint32_t maxBodyPairs{1024};
        uint32_t maxContactConstraints{1024};
        uint32_t maxJobs{2048};
        uint32_t maxBarriers{8};
        size_t tempAllocatorBytes{10u * 1024u * 1024u};
        float3 gravity{0.0f, -9.81f, 0.0f};
    };

    struct PhysicsShapeDesc
    {
        PhysicsShapeType type{PhysicsShapeType::Box};
        float3 halfExtent{0.5f, 0.5f, 0.5f};
        float radius{0.5f};
        float halfHeight{0.5f};
    };

    struct PhysicsBodyPose
    {
        float3 position{0.0f, 0.0f, 0.0f};
        quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct PhysicsBodyDesc
    {
        PhysicsShapeHandle shape{};
        PhysicsBodyPose pose{};
        PhysicsMotionType motion{PhysicsMotionType::Dynamic};
        uint32_t layer{kPhysicsLayerMoving};
        PhysicsActivation activation{PhysicsActivation::Activate};
        float3 linearVelocity{0.0f, 0.0f, 0.0f};
        float3 angularVelocity{0.0f, 0.0f, 0.0f};
    };

    struct PhysicsRayDesc
    {
        float3 origin{0.0f, 0.0f, 0.0f};
        float3 direction{0.0f, 0.0f, -1.0f};
        float maxDistance{1000.0f};
        uint32_t layerMask{~0u};
    };

    struct PhysicsRayHit
    {
        PhysicsBodyHandle body{};
        float3 position{0.0f, 0.0f, 0.0f};
        float3 normal{0.0f, 1.0f, 0.0f};
        float distance{0.0f};
    };

    class JoltPhysicsWorld
    {
    public:
        explicit JoltPhysicsWorld(Core::JobSystem* jobs, PhysicsWorldDesc const& desc = {});
        ~JoltPhysicsWorld();

        JoltPhysicsWorld(JoltPhysicsWorld const&) = delete;
        JoltPhysicsWorld& operator=(JoltPhysicsWorld const&) = delete;
        JoltPhysicsWorld(JoltPhysicsWorld&&) = delete;
        JoltPhysicsWorld& operator=(JoltPhysicsWorld&&) = delete;

        [[nodiscard]] PhysicsStatus CreateShape(PhysicsShapeDesc const& desc, PhysicsShapeHandle& outHandle);
        [[nodiscard]] PhysicsStatus DestroyShape(PhysicsShapeHandle handle);

        [[nodiscard]] PhysicsStatus CreateBody(PhysicsBodyDesc const& desc, PhysicsBodyHandle& outHandle);
        [[nodiscard]] PhysicsStatus DestroyBody(PhysicsBodyHandle handle);
        [[nodiscard]] bool IsBodyValid(PhysicsBodyHandle handle) const;

        [[nodiscard]] PhysicsStatus SetBodyPose(PhysicsBodyHandle handle, PhysicsBodyPose const& pose,
                                                PhysicsActivation activation = PhysicsActivation::Activate);
        [[nodiscard]] PhysicsStatus GetBodyPose(PhysicsBodyHandle handle, PhysicsBodyPose& outPose) const;
        [[nodiscard]] PhysicsStatus SetLinearVelocity(PhysicsBodyHandle handle, float3 const& velocity);
        [[nodiscard]] PhysicsStatus GetLinearVelocity(PhysicsBodyHandle handle, float3& outVelocity) const;
        [[nodiscard]] PhysicsStatus AddForce(PhysicsBodyHandle handle, float3 const& force);
        [[nodiscard]] PhysicsStatus AddImpulse(PhysicsBodyHandle handle, float3 const& impulse);
        [[nodiscard]] PhysicsStatus RayCast(PhysicsRayDesc const& ray, PhysicsRayHit& outHit) const;

        [[nodiscard]] PhysicsStatus Step(float deltaSeconds, int collisionSteps = 1);
        void OptimizeBroadPhase();

        [[nodiscard]] uint32_t GetBodyCount() const { return mBodyCount; }
        [[nodiscard]] uint32_t GetMaxBodies() const { return mDesc.maxBodies; }

    private:
        struct Impl;
        Impl* mImpl{};
        PhysicsWorldDesc mDesc{};
        uint32_t mBodyCount{0};
    };
} // namespace Foundation::Examples
