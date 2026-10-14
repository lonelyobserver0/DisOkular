#pragma once

#include <QPointer>
#include <QWidget>

class DocumentView;
class QLabel;
class QStackedWidget;
class QTreeView;

// Indice del documento corrente: segnalibri del PDF (o del LaTeX compilato).
class OutlinePanel : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinePanel(QWidget *parent = nullptr);

    void setView(DocumentView *view);
    DocumentView *view() const { return m_view; }

    // Evidenzia la voce che contiene la posizione corrente del documento.
    void syncToPosition();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void onModelReset();
    void updatePlaceholder();

    QPointer<DocumentView> m_view;
    QStackedWidget *m_stack;
    QTreeView *m_tree;
    QLabel *m_placeholder;
    QMetaObject::Connection m_resetConn;
    QMetaObject::Connection m_changedConn;
};
