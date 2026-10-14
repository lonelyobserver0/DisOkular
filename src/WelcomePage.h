#pragma once

#include <QWidget>

class History;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPushButton;
class QSortFilterProxyModel;
class QStandardItemModel;

// Pagina mostrata quando non ci sono documenti aperti: nome dell'app e cronologia.
class WelcomePage : public QWidget
{
    Q_OBJECT

public:
    explicit WelcomePage(History *history, QWidget *parent = nullptr);

signals:
    void openRequested(const QString &path);
    void openDialogRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void rebuild();
    void activate(const QModelIndex &index);
    void showContextMenu(const QPoint &pos);
    void updateEmptyState();

    History *m_history;
    QLineEdit *m_filter;
    QListView *m_list;
    QLabel *m_empty;
    QPushButton *m_clear;
    QStandardItemModel *m_model;
    QSortFilterProxyModel *m_proxy;
};
