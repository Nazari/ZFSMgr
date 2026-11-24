#pragma once

// The commands that set up a copy, from what the person chose in the assistant. Pure: no
// process, no widget, so it can be tested. The assistant shows these lines before running
// them —«Commands that will run»—, and runs exactly these: what is shown is what is done.
//
// Three ways:
// - Once: a snapshot now, sent to the destination. It does not update itself.
// - Pulled (recommended): the destination's daemon pulls from the origin
//   (`schedule --origin`). From the destination the original cannot be deleted, even if
//   someone gets into it. The origin only needs one snapshot of each class; the copy keeps its
//   own numbers.
// - Pushed: the origin's daemon sends after each snapshot (`schedule --to … --level`). The copy
//   keeps exactly what the origin keeps.
//
// Credentials and listening addresses are added only when the daemon that has to reach the
// other one lacks them: `peers --listen` restarts a daemon. The interface gives FULL
// credentials only (keys.h), in both directions: a daemon that has none, or only pull ones
// handed by hand in the CLI, gets the full ones (seen 2026-11-22: fc16 knew unib, pull-only,
// and the levelling failed).

#include <QString>
#include <QStringList>

namespace zfsmgr::gui {

struct CopyKeep {
    int daily{7};
    int weekly{4};
    int monthly{6};
    int yearly{1};
};

enum class CopyMode { Once, Pull, Push };

struct CopyRequest {
    QString originMachine;  // connection id
    QString originDataset;  // pool/a/b
    QString destMachine;    // connection id
    QString destDataset;    // pool/x/y: the copy's full name
    CopyMode mode{CopyMode::Pull};
    CopyKeep keep;
    bool originHasSchedule{false};  // the origin already takes snapshots by itself
    // What credentials one daemon has for the other, as `peers` says: empty (none),
    // «pull», «full», or «known» (it has some, from a daemon too old to say which).
    QString destRoleForOrigin;  // anything but «full» gets the full ones
    QString originRoleForDest;  // the same
    QString stamp;              // «20261122-1905», for the one-off snapshot's name
};

// The command lines, in order. Empty when the request makes no sense (same dataset, no name).
QStringList copyCommands(const CopyRequest& r);

// Why a request cannot be planned, in the interface's language; empty when it can.
QString copyProblem(const CopyRequest& r);

}  // namespace zfsmgr::gui
