#include "History.h"

#include <QSettings>

History::History(QObject *parent)
    : QObject(parent)
{
    load();
}

void History::load()
{
    QSettings s;
    const int n = s.beginReadArray("history");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        const QString path = s.value("path").toString();
        if (!path.isEmpty())
            m_entries.append({path, s.value("opened").toDateTime()});
    }
    s.endArray();

    // Prima versione: solo i percorsi, senza data. Si importano una volta.
    if (m_entries.isEmpty() && s.contains("recentFiles")) {
        for (const QString &path : s.value("recentFiles").toStringList())
            m_entries.append({path, QDateTime()});
        s.remove("recentFiles");
        save();
    }
}

void History::save()
{
    QSettings s;
    s.remove("history");
    s.beginWriteArray("history", int(m_entries.size()));
    for (int i = 0; i < m_entries.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("path", m_entries[i].path);
        s.setValue("opened", m_entries[i].opened);
    }
    s.endArray();
}

void History::add(const QString &path)
{
    m_entries.removeIf([&](const Entry &e) { return e.path == path; });
    m_entries.prepend({path, QDateTime::currentDateTime()});
    while (m_entries.size() > MaxEntries)
        m_entries.removeLast();
    save();
    emit changed();
}

void History::remove(const QString &path)
{
    if (m_entries.removeIf([&](const Entry &e) { return e.path == path; }) == 0)
        return;
    save();
    emit changed();
}

void History::clear()
{
    m_entries.clear();
    save();
    emit changed();
}
