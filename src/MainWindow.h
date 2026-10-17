#pragma once

#include <QJsonObject>
#include <QMainWindow>

class DocumentView;
class History;
class QStackedWidget;
class WelcomePage;
class OutlinePanel;
class QDockWidget;
class QAction;
class QLabel;
class QLineEdit;
class QMenu;
class QSpinBox;
class QTabWidget;
class QToolBar;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString &path, const QString &anchor = {});

    static bool isSupported(const QString &path);

    // Richiesta da riga di comando o da un'altra istanza (vedi SingleInstance).
    void handleRequest(const QJsonObject &request);
    // SyncTeX in avanti: `main` è il documento principale (.tex o .pdf), se noto.
    void forwardSearch(const QString &file, int line, int column, const QString &main);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void createActions();
    void openDialog();
    void closeTab(int index);
    void reloadCurrent();
    void toggleFullScreen();
    void chooseEditor();
    void showFindBar();
    void findNext(bool backward);
    void rebuildRecentMenu();
    void updateUi();

    DocumentView *current() const;

    History *m_history;
    QStackedWidget *m_central;
    WelcomePage *m_welcome;
    QTabWidget *m_tabs;
    OutlinePanel *m_outline;
    QDockWidget *m_outlineDock;
    bool m_outlineWanted = true;   // scelta dell'utente, indipendente dalla pagina iniziale
    QToolBar *m_findBar;
    QLineEdit *m_findEdit;
    QSpinBox *m_pageSpin;
    QLabel *m_pageTotal;
    QLabel *m_statusLabel;
    QAction *m_pageSpinAction;
    QAction *m_findAction;
    QAction *m_pageTotalAction;
    QMenu *m_recentMenu;
    QList<QAction *> m_docActions;      // attive solo con un documento aperto
    QList<QAction *> m_pageActions;     // attive solo su documenti a pagine
};
