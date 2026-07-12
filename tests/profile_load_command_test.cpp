#include "models/ProfileLoadCommand.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

void checkProfileLoad(const QString& command,
                      const QString& expectedType,
                      const QString& expectedName,
                      const char* name)
{
    const ProfileLoadCommand profileLoad = parseProfileLoadCommand(command);
    check(profileLoad.valid, name);
    check(profileLoad.type == expectedType, "profile load type capture");
    check(profileLoad.name == expectedName, "profile load name capture");
}

} // namespace

int main()
{
    checkProfileLoad(QStringLiteral("profile global load \"SO2R\""),
                     QStringLiteral("global"),
                     QStringLiteral("SO2R"),
                     "global profile load command parses");
    check(profileLoadMayRebuildRadioTopology(QStringLiteral("global")),
          "global profile load is topology-changing");

    checkProfileLoad(QStringLiteral(" profile TX load \"Low Power\" "),
                     QStringLiteral("tx"),
                     QStringLiteral("Low Power"),
                     "TX profile load command parses case-insensitively");
    check(!profileLoadMayRebuildRadioTopology(QStringLiteral("tx")),
          "TX profile load is not topology-changing");

    checkProfileLoad(QStringLiteral("profile mic load \"Studio Mic\""),
                     QStringLiteral("mic"),
                     QStringLiteral("Studio Mic"),
                     "mic profile load command parses");
    check(!profileLoadMayRebuildRadioTopology(QStringLiteral("mic")),
          "mic profile load is not topology-changing");

    check(!parseProfileLoadCommand(QStringLiteral("profile global save \"SO2R\"")).valid,
          "non-load profile command is ignored");
    check(kProfileLoadDeferredPanFlushDelayMs > kProfileLoadStateWriteHoldMs,
          "deferred pan flush runs after profile-load write hold");
    check(kProfileLoadPostHoldRecoveryDelayMs > kProfileLoadDeferredPanFlushDelayMs,
          "post-hold recovery runs after deferred pan flush");

    // ── isProfileOwnedRadioStateWrite() — the classification contract (#4142) ──
    //
    // sendCmd() DROPS every command this returns true for while the profile-load
    // hold is armed: it returns before a sequence number is allocated, so the
    // command never reaches the wire. These cases pin down exactly which writes
    // are lost, and therefore which ones MUST be deferred by
    // RadioModel::requestPanCenter() rather than handed to sendCmd() and hoped for.

    // A pan center is profile-owned — this is the write that #4142 lost. Direct
    // frequency entry emits an atomic pair; the `slice tune` half survives and the
    // `center=` half was silently dropped, so the slice retuned and the pan did not.
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 center=7.086000")),
          "pan center write is profile-owned (the #4142 drop)");
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 center=7.086000 bandwidth=0.200000")),
          "coupled pan center+bandwidth write is profile-owned (zoom/drag also dropped)");
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 bandwidth=0.200000")),
          "pan bandwidth write is profile-owned");

    // xpixels/ypixels are the ONE exemption, and only because the client alone
    // knows its pixel geometry — the display is broken until they are sent.
    // MainWindow defers and coalesces them (requestPanDimensionsForRadio); this
    // exemption is a necessity, NOT evidence that early pan writes are safe.
    check(!isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 xpixels=1920 ypixels=800")),
          "pan pixel dimensions are exempt (client-owned display geometry)");
    check(!isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 xpixels=1920")),
          "xpixels alone is exempt");

    // A single non-pixel field anywhere in the argument list re-arms the guard:
    // the exemption is all-or-nothing, so center can never ride in on a dimension
    // write's coat-tails.
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 xpixels=1920 center=7.086000")),
          "pixel dimensions mixed with a center write are NOT exempt");

    // Reads are never suppressed — only writes.
    check(!isProfileOwnedRadioStateWrite(QStringLiteral("display pan info 0x40000000")),
          "pan info read is not a profile-owned write");
    check(!isProfileOwnedRadioStateWrite(QStringLiteral("sub slice all")),
          "subscription is not a profile-owned write");

    // `slice tune` is NOT profile-owned: it is the half of the typed-frequency
    // pair that survived the hold. That asymmetry is the bug — the slice moved
    // and the pan could not follow.
    check(!isProfileOwnedRadioStateWrite(QStringLiteral("slice tune 0 7.086000 autopan=0")),
          "slice tune survives the hold (the surviving half of the #4142 pair)");
    check(isProfileOwnedRadioStateWrite(QStringLiteral("slice set 0 rfgain=20")),
          "slice set write is profile-owned");

    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display panafall set 0x40000000 center=7.086000")),
          "panafall write is profile-owned");
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display waterfall set 0x42000000 color_gain=50")),
          "waterfall write is profile-owned");

    return failures == 0 ? 0 : 1;
}
