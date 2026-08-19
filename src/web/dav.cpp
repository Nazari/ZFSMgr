#include "dav.h"

namespace zfsmgr::web::dav {

std::string escapaXml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string multiestado(const std::vector<Recurso>& recursos, const std::string& profundidad) {
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    x += "<D:multistatus xmlns:D=\"DAV:\">\n";
    std::size_t cuantos = recursos.size();
    if (profundidad == "0" && cuantos > 1) {
        cuantos = 1;   // Depth: 0 es «solo este recurso», sin sus hijos
    }
    for (std::size_t i = 0; i < cuantos; ++i) {
        const Recurso& r = recursos[i];
        x += "  <D:response>\n";
        x += "    <D:href>" + escapaXml(r.href) + "</D:href>\n";
        x += "    <D:propstat>\n      <D:prop>\n";
        x += "        <D:displayname>" + escapaXml(r.nombre) + "</D:displayname>\n";
        if (r.coleccion) {
            x += "        <D:resourcetype><D:collection/></D:resourcetype>\n";
        } else {
            x += "        <D:resourcetype/>\n";
            x += "        <D:getcontentlength>" + std::to_string(r.tamano)
                 + "</D:getcontentlength>\n";
        }
        x += "      </D:prop>\n      <D:status>HTTP/1.1 200 OK</D:status>\n";
        x += "    </D:propstat>\n";
        x += "  </D:response>\n";
    }
    x += "</D:multistatus>\n";
    return x;
}

}  // namespace zfsmgr::web::dav
