#pragma once

#include <string>
#include <vector>

#include "connectionprofile.h"

// Which OTHER machines a machine's daemon can talk to.
//
// A daemon on one end sometimes needs to call the one on another by itself, with no client
// in between: GSA levelling against another machine is the case. For that it keeps the mTLS
// credentials of those peers in `/etc/zfsmgr/peers.json`, and **who it is itself**.
//
// **The `self` key is the one that gets forgotten and the one that hurts most.** Without
// it, the daemon does not recognise a target pointing at its own machine as its own: it
// takes the remote path, finds no credentials for itself and logs «no credentials for the
// peer», which is a baffling message when the peer is you. The local levelling branch never
// runs at all. Seen live on 2026-08-21 against a `peers.json` written by an earlier version
// of the client, which did not write it.
//
// This used to live inside `cli/shell.cpp`, and that is why no other client could hand over
// credentials or say what a machine had configured. What is here is everything that can be
// done without touching the network: composing the payload and reading what the daemon
// answers.
namespace zfsmgr::base::peers {

// One line of `--dump-peers`.
struct Peer {
    std::string id;
    std::string host;
    int puerto{0};
};

// What the daemon knows about itself and about the others.
struct View {
    std::string self;          // who it identifies as; empty is the silent failure above
    std::vector<Peer> pares;
};

// Reads the output of `--dump-peers`: one `SELF\t<id>` line and then `<id>\t<host>\t<port>`
// per peer. It tolerates the SELF line being absent, because a daemon older than this
// change does not emit it.
View parse(const std::string& output);

enum class Failure {
    None_,
    NoOtherConnections,   // there is no other one to hand over
    NoTlsMaterial,        // there are, but none of them has certificates
};

struct Handover {
    std::string payloadB64;          // for `--mutate-set-peers`
    std::vector<std::string> names;  // what is handed over, so it can be asked about first
    Failure failure{Failure::None_};
    bool ok() const { return failure == Failure::None_; }
};

// Composes what has to be handed to `target`: ALL the other connections that have TLS
// material, plus `self` = the name by which the client calls that machine.
//
// The target's own connection is excluded on purpose: it would be telling it how to talk to
// itself, and that is what `self` is for.
//
// **`self` is known by the client and only by the client.** The target machine cannot work
// it out: there is no way for it to know under which name whoever is talking to it has it
// written down, and that name is exactly the one that will appear as the target of a
// levelling.
Handover composeHandover(const std::vector<ConnectionProfile>& profiles,
                       const std::string& target);

std::string labelOf(Failure f);

// The three addresses the daemon accepts to listen on.
//
// Not an arbitrary list: the client arrives through a tunnel against 127.0.0.1, so binding
// to a single address would cut it off. The daemon rejects the rest, and having the same
// list here means only the valid ones get offered, instead of letting the call fail.
bool isValidBindAddress(const std::string& address);
std::vector<std::string> bindAddresses();

}  // namespace zfsmgr::base::peers
