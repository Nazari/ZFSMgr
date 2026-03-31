#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace zfsmgr::daemonrpc {

struct RpcRequest {
    QString id;
    QString method;
    QJsonObject params;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("id"), id);
        root.insert(QStringLiteral("method"), method);
        root.insert(QStringLiteral("params"), params);
        return root;
    }

    QByteArray toJson() const {
        return QJsonDocument(toJsonObject()).toJson(QJsonDocument::Compact);
    }
};

struct RpcResult {
    int code{200};
    QString message;
    QString fallback;
    QJsonObject payload;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("code"), code);
        root.insert(QStringLiteral("message"), message);
        if (!payload.isEmpty()) {
            root.insert(QStringLiteral("payload"), payload);
        }
        if (!fallback.trimmed().isEmpty()) {
            root.insert(QStringLiteral("fallback"), fallback.trimmed());
        }
        return root;
    }
};

struct RpcResponse {
    QString id;
    RpcResult result;
    bool hasError{false};
    int errorCode{0};
    QString errorMessage;
    QJsonObject errorDetails;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("id"), id);
        root.insert(QStringLiteral("result"),
                    hasError ? QJsonValue(QJsonValue::Null) : QJsonValue(result.toJsonObject()));
        if (hasError) {
            QJsonObject error;
            error.insert(QStringLiteral("code"), errorCode);
            error.insert(QStringLiteral("message"), errorMessage);
            if (!errorDetails.isEmpty()) {
                error.insert(QStringLiteral("details"), errorDetails);
            }
            root.insert(QStringLiteral("error"), error);
        } else {
            root.insert(QStringLiteral("error"), QJsonValue(QJsonValue::Null));
        }
        return root;
    }

    QByteArray toJson() const {
        return QJsonDocument(toJsonObject()).toJson(QJsonDocument::Compact);
    }

    static bool fromJson(const QByteArray& body, RpcResponse* out, QString* errorText) {
        if (!out) {
            if (errorText) {
                *errorText = QStringLiteral("output response pointer is null");
            }
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (errorText) {
                *errorText = QStringLiteral("daemon response invalid JSON");
            }
            return false;
        }
        const QJsonObject root = doc.object();
        out->id = root.value(QStringLiteral("id")).toString();
        const QJsonValue resultValue = root.value(QStringLiteral("result"));
        if (resultValue.isObject()) {
            const QJsonObject resultObj = resultValue.toObject();
            out->result.code = resultObj.value(QStringLiteral("code")).toInt(200);
            out->result.message = resultObj.value(QStringLiteral("message")).toString();
            out->result.fallback = resultObj.value(QStringLiteral("fallback")).toString();
            out->result.payload = resultObj.value(QStringLiteral("payload")).toObject();
        } else {
            out->result = RpcResult{};
        }
        const QJsonValue errorValue = root.value(QStringLiteral("error"));
        out->hasError = errorValue.isObject();
        if (out->hasError) {
            const QJsonObject errorObj = errorValue.toObject();
            out->errorCode = errorObj.value(QStringLiteral("code")).toInt();
            out->errorMessage = errorObj.value(QStringLiteral("message")).toString();
            out->errorDetails = errorObj.value(QStringLiteral("details")).toObject();
        } else {
            out->errorCode = 0;
            out->errorMessage.clear();
            out->errorDetails = {};
        }
        if (out->id.trimmed().isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("daemon response missing id");
            }
            return false;
        }
        if (out->hasError && out->errorMessage.trimmed().isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("daemon response missing error message");
            }
            return false;
        }
        return true;
    }
};

struct PoolInfoJson {
    QString pool;
    QString state;
    int version{0};
    QString guid;
    bool importable{false};
    QString comment;
    QStringList children;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("pool"), pool);
        root.insert(QStringLiteral("state"), state);
        root.insert(QStringLiteral("version"), version);
        root.insert(QStringLiteral("guid"), guid);
        root.insert(QStringLiteral("importable"), importable);
        root.insert(QStringLiteral("comment"), comment);
        QJsonArray childArray;
        for (const QString& child : children) {
            childArray.push_back(child);
        }
        root.insert(QStringLiteral("children"), childArray);
        return root;
    }
};

struct PoolDetailsJson {
    QString pool;
    QVector<QStringList> propsRows; // property,value,source
    QString statusText;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("pool"), pool);
        QJsonArray rows;
        for (const QStringList& row : propsRows) {
            QJsonArray rowArray;
            for (const QString& cell : row) {
                rowArray.push_back(cell);
            }
            rows.push_back(rowArray);
        }
        root.insert(QStringLiteral("propsRows"), rows);
        root.insert(QStringLiteral("statusText"), statusText);
        return root;
    }
};

struct DSInfoJson {
    QString dataset;
    QString type;
    QString mountpoint;
    QString canmount;
    QString mounted;
    QString encryption;
    bool mountpointVisible{false};
    QStringList snapshots;
    QJsonObject properties;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("dataset"), dataset);
        root.insert(QStringLiteral("type"), type);
        root.insert(QStringLiteral("mountpoint"), mountpoint);
        root.insert(QStringLiteral("canmount"), canmount);
        root.insert(QStringLiteral("mounted"), mounted);
        root.insert(QStringLiteral("encryption"), encryption);
        root.insert(QStringLiteral("mountpointVisible"), mountpointVisible);
        QJsonArray snapArray;
        for (const QString& snapshot : snapshots) {
            snapArray.push_back(snapshot);
        }
        root.insert(QStringLiteral("snapshots"), snapArray);
        root.insert(QStringLiteral("properties"), properties);
        return root;
    }
};

struct GsaDestinationJson {
    QString connection;
    QString pool;
    QString dataset;

    QJsonObject toJsonObject() const {
        return {
            {QStringLiteral("connection"), connection},
            {QStringLiteral("pool"), pool},
            {QStringLiteral("dataset"), dataset},
        };
    }
};

struct PermissionGrantJson {
    QString scope;
    QString targetType;
    QString targetName;
    QStringList permissions;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("scope"), scope);
        root.insert(QStringLiteral("targetType"), targetType);
        root.insert(QStringLiteral("targetName"), targetName);
        QJsonArray perms;
        for (const QString& perm : permissions) {
            perms.push_back(perm);
        }
        root.insert(QStringLiteral("permissions"), perms);
        return root;
    }
};

struct PermissionSetJson {
    QString name;
    QStringList permissions;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("name"), name);
        QJsonArray perms;
        for (const QString& perm : permissions) {
            perms.push_back(perm);
        }
        root.insert(QStringLiteral("permissions"), perms);
        return root;
    }
};

struct PermissionsJson {
    QString dataset;
    QVector<PermissionGrantJson> localGrants;
    QVector<PermissionGrantJson> descendantGrants;
    QVector<PermissionGrantJson> localDescendantGrants;
    QStringList createPermissions;
    QVector<PermissionSetJson> permissionSets;
    QStringList systemUsers;
    QStringList systemGroups;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("dataset"), dataset);
        auto grantsToArray = [](const QVector<PermissionGrantJson>& grants) {
            QJsonArray arr;
            for (const PermissionGrantJson& grant : grants) {
                arr.push_back(grant.toJsonObject());
            }
            return arr;
        };
        root.insert(QStringLiteral("localGrants"), grantsToArray(localGrants));
        root.insert(QStringLiteral("descendantGrants"), grantsToArray(descendantGrants));
        root.insert(QStringLiteral("localDescendantGrants"), grantsToArray(localDescendantGrants));
        QJsonArray createArray;
        for (const QString& perm : createPermissions) {
            createArray.push_back(perm);
        }
        root.insert(QStringLiteral("createPermissions"), createArray);
        QJsonArray setsArray;
        for (const PermissionSetJson& set : permissionSets) {
            setsArray.push_back(set.toJsonObject());
        }
        root.insert(QStringLiteral("permissionSets"), setsArray);
        QJsonArray usersArray;
        for (const QString& user : systemUsers) {
            usersArray.push_back(user);
        }
        root.insert(QStringLiteral("systemUsers"), usersArray);
        QJsonArray groupsArray;
        for (const QString& group : systemGroups) {
            groupsArray.push_back(group);
        }
        root.insert(QStringLiteral("systemGroups"), groupsArray);
        return root;
    }
};

struct HoldJson {
    QString tag;
    QString timestamp;

    QJsonObject toJsonObject() const {
        return {
            {QStringLiteral("tag"), tag},
            {QStringLiteral("timestamp"), timestamp},
        };
    }
};

struct HoldsJson {
    QString dataset;
    QVector<HoldJson> holds;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("dataset"), dataset);
        QJsonArray arr;
        for (const HoldJson& hold : holds) {
            arr.push_back(hold.toJsonObject());
        }
        root.insert(QStringLiteral("holds"), arr);
        return root;
    }
};

struct GsaPlanJson {
    bool enabled{false};
    bool recursive{false};
    QStringList classes;
    QVector<GsaDestinationJson> destinations;

    QJsonObject toJsonObject() const {
        QJsonObject root;
        root.insert(QStringLiteral("enabled"), enabled);
        root.insert(QStringLiteral("recursive"), recursive);
        QJsonArray classArray;
        for (const QString& klass : classes) {
            classArray.push_back(klass);
        }
        root.insert(QStringLiteral("classes"), classArray);
        QJsonArray destinationArray;
        for (const GsaDestinationJson& destination : destinations) {
            destinationArray.push_back(destination.toJsonObject());
        }
        root.insert(QStringLiteral("destinations"), destinationArray);
        return root;
    }
};

} // namespace zfsmgr::daemonrpc
