#include "coopstory/bridge/NpcAnimation.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace coopstory::bridge {
namespace {
bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::abs(v.x) <= 100'000.0F && std::abs(v.y) <= 100'000.0F && std::abs(v.z) <= 100'000.0F;
}
void Write(std::vector<std::uint8_t>& b, std::uint64_t v, const int count) {
    for (int i = 0; i < count; ++i) { b.push_back(static_cast<std::uint8_t>(v & 255U)); v >>= 8U; }
}
std::uint64_t Read(std::span<const std::uint8_t> b, std::size_t& at, const int count) {
    std::uint64_t v{};
    for (int i = 0; i < count; ++i) v |= static_cast<std::uint64_t>(b[at++]) << (i * 8);
    return v;
}
}

bool IsValidNpcAnimation(const NpcAnimationPayload& p) noexcept {
    return p.entityId.IsValid() && p.revision != 0U && p.modelHash != 0U &&
        (p.flags & ~255U) == 0U && (p.events & ~3U) == 0U &&
        p.leaseMs >= 100U && p.leaseMs <= 2'000U &&
        ((p.events & 1U) == 0U || p.weaponHash != 0U) &&
        ((p.flags & 1U) == 0U || p.weaponHash != 0U) &&
        ((p.flags & 32U) == 0U || (p.flags == 32U && p.events == 0U)) &&
        Finite(p.target) && Finite(p.velocity) && std::isfinite(p.healthFraction) &&
        p.healthFraction >= 0.0F && p.healthFraction <= 1.0F;
}
std::vector<std::uint8_t> EncodeNpcAnimation(const NpcAnimationPayload& p) {
    if (!IsValidNpcAnimation(p)) throw std::invalid_argument("invalid NPC animation payload");
    std::vector<std::uint8_t> b; b.reserve(kNpcAnimationPayloadSize);
    Write(b, p.entityId.Value(), 8); Write(b, p.revision, 4);
    Write(b, 1U, 1); Write(b, 0U, 1);
    Write(b, p.flags, 2); Write(b, p.events, 2); Write(b, p.leaseMs, 2);
    Write(b, p.weaponHash, 4); Write(b, p.modelHash, 4);
    for (const auto f : {p.target.x, p.target.y, p.target.z, p.velocity.x,
                         p.velocity.y, p.velocity.z, p.healthFraction})
        Write(b, std::bit_cast<std::uint32_t>(f), 4);
    return b;
}
std::optional<NpcAnimationPayload> DecodeNpcAnimation(const std::span<const std::uint8_t> b) {
    if (b.size() != kNpcAnimationPayloadSize || b[12] != 1U || b[13] != 0U) return std::nullopt;
    std::size_t at{};
    NpcAnimationPayload p;
    p.entityId = NetEntityId{Read(b, at, 8)};
    p.revision = static_cast<std::uint32_t>(Read(b, at, 4)); at += 2;
    p.flags = static_cast<std::uint16_t>(Read(b, at, 2));
    p.events = static_cast<std::uint16_t>(Read(b, at, 2));
    p.leaseMs = static_cast<std::uint16_t>(Read(b, at, 2));
    p.weaponHash = static_cast<std::uint32_t>(Read(b, at, 4));
    p.modelHash = static_cast<std::uint32_t>(Read(b, at, 4));
    for (auto* f : {&p.target.x, &p.target.y, &p.target.z, &p.velocity.x,
                    &p.velocity.y, &p.velocity.z, &p.healthFraction})
        *f = std::bit_cast<float>(static_cast<std::uint32_t>(Read(b, at, 4)));
    return IsValidNpcAnimation(p) ? std::optional{p} : std::nullopt;
}

void NpcAnimationCapture::Observe(const NetEntityId id, const NpcAnimationSample& s, const std::uint64_t now) {
    const bool dead = (s.flags & 32U) != 0U;
    const bool sameWeapon = initialized_ && s.weaponHash == previous_.weaponHash;
    int shots = 0;
    if (initialized_ && !dead && s.weaponHash != 0U) {
        if (sameWeapon && s.clipAmmo >= 0 && previous_.clipAmmo >= 0 && s.clipAmmo < previous_.clipAmmo &&
            (s.flags & 1U) == 0U) {
            shots = std::min(previous_.clipAmmo - s.clipAmmo, 8);
        } else if (s.firing && !previous_.firing) shots = 1;
    }
    const bool hit = initialized_ && !dead && s.healthFraction + 0.001F < previous_.healthFraction;
    const bool changed = !initialized_ || s.flags != previous_.flags || !sameWeapon;
    const bool heartbeat = now < previousSentMs_ || now - previousSentMs_ >= 500U;
    if (changed || shots != 0 || hit || heartbeat) {
        for (int i = 0; i < std::max(shots, 1); ++i) {
            if (++revision_ == 0U) ++revision_;
            NpcAnimationPayload p{id, revision_, s.flags,
                static_cast<std::uint16_t>((shots != 0 ? 1U : 0U) | (hit && i == 0 ? 2U : 0U)),
                1'000U, s.weaponHash, s.modelHash, s.target, s.velocity, s.healthFraction};
            if (dead) { p.flags = 32U; p.events = 0U; }
            if (IsValidNpcAnimation(p)) {
                if (pending_.size() == kNpcAnimationQueueCapacity) { pending_.pop_front(); ++dropped; }
                pending_.push_back({p, now});
            }
        }
        previousSentMs_ = now;
    }
    previous_ = s; initialized_ = true;
}
const TimedNpcAnimation* NpcAnimationCapture::Pending(const std::uint64_t now) {
    // Leave at least the wire minimum lease; retries must never renew an old action.
    while (!pending_.empty() && (now < pending_.front().atMs ||
        now - pending_.front().atMs > pending_.front().payload.leaseMs - 100U)) {
        pending_.pop_front(); ++dropped;
    }
    return pending_.empty() ? nullptr : &pending_.front();
}
void NpcAnimationCapture::Acknowledge() { if (!pending_.empty()) pending_.pop_front(); }
void NpcAnimationCapture::Rebaseline() noexcept { initialized_ = false; pending_.clear(); }
bool NpcAnimationInbox::Push(const NpcAnimationPayload& p, const std::uint64_t now) {
    if (!IsValidNpcAnimation(p)) { ++rejected; return false; }
    const auto disposition = revisions_.Observe(p.revision);
    if (disposition == SequenceDisposition::Duplicate || disposition == SequenceDisposition::Stale) {
        ++rejected; return false;
    }
    if (pending_.size() == kNpcAnimationQueueCapacity) { pending_.pop_front(); ++expired; }
    pending_.push_back({p, now}); return true;
}
std::optional<NpcAnimationPayload> NpcAnimationInbox::Pop(const std::uint64_t now) {
    while (!pending_.empty()) {
        const auto entry = pending_.front(); pending_.pop_front();
        if (now < entry.atMs || now - entry.atMs >= entry.payload.leaseMs) { ++expired; continue; }
        auto result = entry.payload;
        result.leaseMs = static_cast<std::uint16_t>(result.leaseMs - (now - entry.atMs));
        return result;
    }
    return std::nullopt;
}
NpcAnimationOwner SelectNpcAnimationOwner(const std::uint16_t f) noexcept {
    if (f & 32U) return NpcAnimationOwner::Dead;
    if (f & (2U | 4U)) return NpcAnimationOwner::Physical;
    if (f & 16U) return NpcAnimationOwner::Mounted;
    if (f & (64U | 128U)) return NpcAnimationOwner::Traversal;
    if (f & 1U) return NpcAnimationOwner::Reload;
    if (f & 8U) return NpcAnimationOwner::Melee;
    return NpcAnimationOwner::None;
}
}  // namespace coopstory::bridge
