#include "network.h"

#include "secretcipher.h"
#include "strutil.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <set>

namespace zfsmgr::base::network {

namespace J = zfsmgr::base::json;

namespace {

std::string hmacSha256(const std::string& key, const std::string& data) {
    unsigned char buf[EVP_MAX_MD_SIZE] = {0};
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), buf, &len);
    return std::string(reinterpret_cast<const char*>(buf), len);
}

bool sameInConstantTime(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char dif = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dif |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return dif == 0;
}

J::Value sorted(const J::Value& v) {
    if (v.isObject()) {
        J::Object o = v.toObject();
        std::sort(o.begin(), o.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& kv : o) {
            kv.second = sorted(kv.second);
        }
        return J::Value(o);
    }
    if (v.isArray()) {
        J::Array a;
        for (const J::Value& x : v.toArray()) {
            a.push_back(sorted(x));
        }
        return J::Value(a);
    }
    return v;
}

// A secret field: empty, or encrypted. A value in the clear must never reach the daemons.
bool secretOk(const std::string& s) {
    return s.empty() || SecretCipher::isEncrypted(s);
}

J::Value machineToJson(const Machine& m) {
    J::Value o;
    o.set("uid", J::Value(m.uid));
    o.set("name", J::Value(m.name));
    o.set("os", J::Value(m.os));
    J::Array addrs;
    for (const std::string& a : m.addresses) {
        addrs.push_back(J::Value(a));
    }
    o.set("addresses", J::Value(addrs));
    J::Value ssh;
    ssh.set("user", J::Value(m.sshUser));
    ssh.set("port", J::Value(m.sshPort));
    ssh.set("family", J::Value(m.sshFamily));
    o.set("ssh", ssh);
    o.set("sudo", J::Value(m.sudo));
    J::Value secrets;
    secrets.set("password", J::Value(m.password));
    o.set("secrets", secrets);
    J::Value tls;
    tls.set("port", J::Value(m.tlsPort));
    tls.set("server_cert", J::Value(m.tlsServerCert));
    tls.set("client_cert", J::Value(m.tlsClientCert));
    tls.set("client_key", J::Value(m.tlsClientKey));
    o.set("tls", tls);
    return o;
}

}  // namespace

std::string canonical(const J::Value& v) {
    return J::toCompact(sorted(v));
}

J::Value toJson(const Document& d, bool withMac) {
    J::Value o;
    o.set("schema", J::Value(d.schema));
    o.set("revision", J::Value(d.revision));
    o.set("written_by", J::Value(d.writtenBy));
    o.set("written_at", J::Value(d.writtenAt));
    o.set("master", d.master);
    J::Array ms;
    for (const Machine& m : d.machines) {
        ms.push_back(machineToJson(m));
    }
    o.set("machines", J::Value(ms));
    if (withMac) {
        o.set("mac", J::Value(d.mac));
    }
    return o;
}

bool fromJson(const J::Value& v, Document& d, std::string& error) {
    d = Document{};
    if (!v.isObject()) {
        error = "not an object";
        return false;
    }
    d.schema = static_cast<int>(v["schema"].toInt(0));
    if (d.schema != 1) {
        error = "unknown schema " + std::to_string(d.schema);
        return false;
    }
    d.revision = v["revision"].toInt(-1);
    if (d.revision < 0) {
        error = "no revision";
        return false;
    }
    d.writtenBy = v["written_by"].toString();
    d.writtenAt = v["written_at"].toInt(0);
    d.master = v["master"];
    d.mac = v["mac"].toString();
    std::set<std::string> uids;
    std::set<std::string> names;
    for (const J::Value& x : v["machines"].toArray()) {
        Machine m;
        m.uid = x["uid"].toString();
        m.name = x["name"].toString();
        m.os = x["os"].toString();
        for (const J::Value& a : x["addresses"].toArray()) {
            if (!a.toString().empty()) {
                m.addresses.push_back(a.toString());
            }
        }
        m.sshUser = x["ssh"]["user"].toString();
        m.sshPort = static_cast<int>(x["ssh"]["port"].toInt(22));
        m.sshFamily = x["ssh"]["family"].toString("auto");
        m.sudo = x["sudo"].toBool(false);
        m.password = x["secrets"]["password"].toString();
        m.tlsPort = static_cast<int>(x["tls"]["port"].toInt(47653));
        m.tlsServerCert = x["tls"]["server_cert"].toString();
        m.tlsClientCert = x["tls"]["client_cert"].toString();
        m.tlsClientKey = x["tls"]["client_key"].toString();
        if (m.uid.empty() || m.name.empty()) {
            error = "a machine without uid or name";
            return false;
        }
        if (!uids.insert(m.uid).second || !names.insert(toLowerAscii(m.name)).second) {
            error = "two machines share a uid or a name: " + m.name;
            return false;
        }
        if (!secretOk(m.password) || !secretOk(m.tlsClientKey) || !secretOk(m.tlsClientCert)
            || !secretOk(m.tlsServerCert)) {
            error = "a secret in the clear: " + m.name;
            return false;
        }
        d.machines.push_back(std::move(m));
    }
    return true;
}

std::string macOf(const Document& d, const std::string& dataKey) {
    const std::string key = hmacSha256(dataKey, "zfsmgr-network-mac-v1");
    return base64Encode(hmacSha256(key, canonical(toJson(d, /*withMac=*/false))));
}

void sign(Document& d, const std::string& dataKey) {
    d.mac = macOf(d, dataKey);
}

bool verify(const Document& d, const std::string& dataKey) {
    return !dataKey.empty() && !d.mac.empty() && sameInConstantTime(d.mac, macOf(d, dataKey));
}

Verdict judge(const Document& incoming, const std::string& dataKey, long long cachedRevision,
              const std::string& cachedMac) {
    if (!verify(incoming, dataKey)) {
        return Verdict::BadMac;
    }
    if (incoming.revision > cachedRevision) {
        return Verdict::Newer;
    }
    if (incoming.revision < cachedRevision) {
        return Verdict::Older;
    }
    return sameInConstantTime(incoming.mac, cachedMac) ? Verdict::Same : Verdict::Diverged;
}

bool offeredWins(long long keptRevision, const std::string& keptMac, long long offeredRevision,
                 const std::string& offeredMac) {
    if (offeredRevision != keptRevision) {
        return offeredRevision > keptRevision;
    }
    return offeredMac > keptMac;
}

bool fromClientConfig(const J::Value& config, const J::Value& trustStore,
                      const std::string& thisHost, const std::string& thisUid, long long now,
                      Document& d, LocalSettings& local, std::string& error) {
    d = Document{};
    local = LocalSettings{};
    // The master block: the slots as they are, and the check beside them.
    J::Value master = config["master"];
    if (!master.isObject() || master["slots"].toArray().empty()) {
        error = "the configuration has no master password";
        return false;
    }
    if (config.contains("master_check")) {
        master.set("check", config["master_check"]);
    }
    d.master = master;
    std::map<std::string, J::Value> trustById;
    for (const J::Value& t : trustStore["connections"].toArray()) {
        trustById[t["id"].toString()] = t;
    }
    std::set<std::string> uids;
    std::set<std::string> names;
    for (const J::Value& c : config["connections"].toArray()) {
        const std::string id = c["id"].toString();
        Machine m;
        m.uid = c["machine_uid"].toString();
        if (m.uid.empty()) {
            error = "the connection «" + id + "» has no machine uid: refresh it first";
            return false;
        }
        const bool isLocal = toUpperAscii(c["conn_type"].toString()) == "LOCAL" || id == "local";
        m.name = isLocal ? thisHost : c["name"].toString(id);
        if (m.name.empty()) {
            m.name = id;
        }
        m.os = c["os_type"].toString();
        const std::string host = isLocal ? thisHost : c["host"].toString();
        if (!host.empty()) {
            m.addresses.push_back(host);
        }
        m.sshUser = c["username"].toString();
        m.sshPort = isLocal ? 22 : static_cast<int>(c["port"].toInt(22));
        if (m.sshPort <= 0) {
            m.sshPort = 22;
        }
        m.sshFamily = c["ssh_address_family"].toString("auto");
        m.sudo = c["use_sudo"].toBool(false);
        m.password = c["password"].toString();
        const auto t = trustById.find(id);
        if (t != trustById.end()) {
            m.tlsPort = static_cast<int>(t->second["daemon_tls_port"].toInt(47653));
            m.tlsServerCert = t->second["daemon_tls_server_cert_pem"].toString();
            m.tlsClientCert = t->second["daemon_tls_client_cert_pem"].toString();
            m.tlsClientKey = t->second["daemon_tls_client_key_pem"].toString();
        }
        if (!secretOk(m.password) || !secretOk(m.tlsClientKey) || !secretOk(m.tlsClientCert)
            || !secretOk(m.tlsServerCert)) {
            error = "the connection «" + id + "» has a secret in the clear";
            return false;
        }
        if (!uids.insert(m.uid).second) {
            error = "two connections are the same machine: «" + id + "»";
            return false;
        }
        if (!names.insert(toLowerAscii(m.name)).second) {
            error = "two machines would be called «" + m.name + "»";
            return false;
        }
        if (const std::string k = c["key_path"].toString(); !k.empty()) {
            local.keyPaths[m.uid] = k;
        }
        d.machines.push_back(std::move(m));
    }
    d.revision = 1;
    d.writtenBy = thisUid;
    d.writtenAt = now;
    return true;
}

void toClientConfig(const Document& d, const LocalSettings& local, const std::string& thisUid,
                    J::Value& config, J::Value& trustStore) {
    config = J::Value();
    trustStore = J::Value();
    J::Value master = d.master;
    const J::Value check = master["check"];
    master.remove("check");
    config.set("master", master);
    if (!check.isNull()) {
        config.set("master_check", check);
    }
    J::Array conns;
    J::Array trust;
    for (const Machine& m : d.machines) {
        const bool here = !thisUid.empty() && m.uid == thisUid;
        J::Value c;
        c.set("id", J::Value(m.name));
        c.set("name", J::Value(m.name));
        c.set("conn_type", J::Value(std::string(here ? "LOCAL" : "SSH")));
        std::string host = m.addresses.empty() ? std::string() : m.addresses.front();
        if (const auto p = local.preferredAddress.find(m.uid); p != local.preferredAddress.end()) {
            host = p->second;
        }
        c.set("host", J::Value(here ? std::string("localhost") : host));
        c.set("port", J::Value(here ? 0 : m.sshPort));
        c.set("machine_uid", J::Value(m.uid));
        c.set("username", J::Value(m.sshUser));
        c.set("use_sudo", J::Value(m.sudo));
        c.set("password", J::Value(m.password));
        c.set("os_type", J::Value(m.os));
        c.set("ssh_address_family", J::Value(m.sshFamily));
        const auto k = local.keyPaths.find(m.uid);
        c.set("key_path", J::Value(k == local.keyPaths.end() ? std::string() : k->second));
        conns.push_back(c);
        if (!m.tlsServerCert.empty() || !m.tlsClientCert.empty() || !m.tlsClientKey.empty()) {
            J::Value t;
            t.set("id", J::Value(m.name));
            t.set("machine_uid", J::Value(m.uid));
            t.set("daemon_tls_port", J::Value(m.tlsPort));
            t.set("daemon_tls_server_cert_pem", J::Value(m.tlsServerCert));
            t.set("daemon_tls_client_cert_pem", J::Value(m.tlsClientCert));
            t.set("daemon_tls_client_key_pem", J::Value(m.tlsClientKey));
            trust.push_back(t);
        }
    }
    config.set("connections", J::Value(conns));
    trustStore.set("connections", J::Value(trust));
}

}  // namespace zfsmgr::base::network
