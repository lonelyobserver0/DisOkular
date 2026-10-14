#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>

// Cronologia dei documenti aperti, dal più recente. Persistente in QSettings.
class History : public QObject
{
    Q_OBJECT

public:
    struct Entry {
        QString path;
        QDateTime opened;   // non valida per le voci importate dal vecchio formato
    };

    static constexpr int MaxEntries = 50;

    explicit History(QObject *parent = nullptr);

    QList<Entry> entries() const { return m_entries; }

    void add(const QString &path);
    void remove(const QString &path);
    void clear();

signals:
    void changed();

private:
    void load();
    void save();

    QList<Entry> m_entries;
};
