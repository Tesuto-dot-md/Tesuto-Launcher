#include "InstanceStore.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>

InstanceStore::InstanceStore(QString rootGameDir)
    : root_(std::move(rootGameDir))
{
    QDir().mkpath(root_);
    QDir().mkpath(root_ + "/instances");
}

QString InstanceStore::jsonPath() const
{
    return root_ + "/instances.json";
}

QJsonObject InstanceStore::loadJson() const
{
    QFile f(jsonPath());
    if (f.open(QIODevice::ReadOnly)) {
        auto doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) return doc.object();
    }
    return QJsonObject{{"instances", QJsonArray{}}};
}

bool InstanceStore::saveJson(const QJsonObject& o) const
{
    QSaveFile f(jsonPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

QString InstanceStore::slugify(const QString& name)
{
    QString s = name.toLower();
    QString out; out.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber()) out.append(c);
        else if (c.isSpace() || c == '-' || c == '_' || c == '.') out.append('-');
    }
    while (out.contains("--")) out.replace("--", "-");
    if (out.startsWith('-')) out.remove(0,1);
    if (out.endsWith('-')) out.chop(1);
    if (out.isEmpty()) out = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return out;
}

QList<Instance> InstanceStore::list() const
{
    QList<Instance> res;
    auto root = loadJson();
    auto arr = root.value("instances").toArray();
    for (const auto& v : arr) {
        if (v.isObject()) res.push_back(Instance::fromJson(v.toObject()));
    }
    return res;
}

bool InstanceStore::add(const Instance& iIn)
{
    Instance i = iIn;
    if (i.id.isEmpty()) i.id = slugify(i.name);
    auto root = loadJson();
    auto arr = root.value("instances").toArray();
    // уникальность id
    for (const auto& v : arr) {
        if (v.isObject() && v.toObject().value("id").toString() == i.id)
            return false;
    }
    arr.push_back(i.toJson());
    root.insert("instances", arr);
    QDir().mkpath(pathFor(i));
    return saveJson(root);
}

bool InstanceStore::remove(const QString& id)
{
    auto root = loadJson();
    auto arr = root.value("instances").toArray();
    QJsonArray out;
    bool found=false;
    for (const auto& v : arr) {
        auto o = v.toObject();
        if (o.value("id").toString() == id) { found=true; continue; }
        out.push_back(o);
    }
    if (!found) return false;
    root.insert("instances", out);
    return saveJson(root);
}

bool InstanceStore::update(const Instance& i)
{
    auto root = loadJson();
    auto arr = root.value("instances").toArray();
    bool found=false;
    QJsonArray out;
    for (const auto& v : arr) {
        auto o = v.toObject();
        if (o.value("id").toString() == i.id) { out.push_back(i.toJson()); found=true; }
        else out.push_back(o);
    }
    if (!found) return false;
    root.insert("instances", out);
    return saveJson(root);
}

QString InstanceStore::pathFor(const Instance& i) const
{
    return root_ + "/instances/" + i.id;
}
