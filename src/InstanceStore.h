#pragma once
#include <QtCore>

struct Instance {
    QString id;        // slug/uuid-директория
    QString name;
    QString versionId; // "1.21.1" и т.п.
    QString group;     // произвольная группа

    QJsonObject toJson() const {
        return QJsonObject{
            {"id", id},
            {"name", name},
            {"versionId", versionId},
            {"group", group},
        };
    }
    static Instance fromJson(const QJsonObject& o) {
        Instance i;
        i.id        = o.value("id").toString();
        i.name      = o.value("name").toString();
        i.versionId = o.value("versionId").toString();
        i.group     = o.value("group").toString();
        return i;
    }
};

class InstanceStore {
public:
    explicit InstanceStore(QString rootGameDir);

    QList<Instance> list() const;
    bool add(const Instance& i);
    bool remove(const QString& id);
    bool update(const Instance& i);

    QString rootDir() const { return root_; }
    QString pathFor(const Instance& i) const; // каталог игры под инстанс

private:
    QString root_;
    QString jsonPath() const;
    QJsonObject loadJson() const;
    bool saveJson(const QJsonObject& o) const;
    static QString slugify(const QString& name);
};
