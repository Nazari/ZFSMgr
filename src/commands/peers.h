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
    // El identificador de MÁQUINA, que no depende de quién mire.
    std::string uid;
};

// What the daemon knows about itself and about the others.
struct View {
    std::string self;          // who it identifies as; empty is the silent failure above
    std::vector<Peer> pares;
    // El uid que esa máquina tiene guardado como suyo. Vacío = nunca se le entregó.
    std::string selfUid;
    // Y el que la máquina lee DE SÍ MISMA, que no se lo ha dicho nadie. Lo que importa es si
    // coinciden: si no, esa configuración se escribió mirando a otra máquina.
    std::string realUid;
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
                       const std::string& destination,
                       // El uid de la máquina de destino, tal y como lo tiene el cliente.
                       // Vacío si nunca se refrescó esa conexión.
                       const std::string& destinationUid);

// --- ¿El destino de una nivelación es ESTA máquina?
//
// Vive aquí, en la capa base y con pruebas, y no dentro del daemon, porque es una REGLA: de
// ella depende que un envío salga por la red o se quede en casa, y las reglas enterradas en
// una función de mil líneas es donde se esconden los fallos que nadie ve. En este proyecto
// ya ha pasado.
//
// Decide SIEMPRE el identificador de máquina, nunca el nombre:
//
//   1. Si el nombre es el de un PAR y ese par trae uid, manda ese uid. Es el caso que
//      arregla el defecto: el cliente que escribió la programación llama a esta máquina de
//      una forma y el que entregó las credenciales la llamó de otra.
//   2. Si no, y el nombre es con el que se entregaron las credenciales, manda el uid que se
//      entregó —comprobando que de verdad es el mío—.
//   3. Sin uid por ninguna parte, NO: mejor no nivelar que nivelar al sitio equivocado.
//      `peers --push` exige el uid de la máquina a la que entrega.
struct SelfMatch {
    std::string selfName;              // con qué nombre se le entregaron las credenciales
    std::string selfUid;               // qué uid se le entregó
    std::string realUid;               // el que la máquina lee de sí misma
    std::vector<Peer> peers;
};

bool destinationIsSelf(const std::string& connId, const SelfMatch& m);

std::string labelOf(Failure f);

// The three addresses the daemon accepts to listen on.
//
// Not an arbitrary list: the client arrives through a tunnel against 127.0.0.1, so binding
// to a single address would cut it off. The daemon rejects the rest, and having the same
// list here means only the valid ones get offered, instead of letting the call fail.
bool isValidBindAddress(const std::string& address);
std::vector<std::string> bindAddresses();

}  // namespace zfsmgr::base::peers
