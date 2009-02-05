/* This file is (c) 2008-2009 Konstantin Isakov <ikm@users.berlios.de>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "mainwindow.hh"
#include "sources.hh"
#include "groups.hh"
#include "bgl.hh"
#include "stardict.hh"
#include "lsa.hh"
#include "dsl.hh"
#include "dictlock.hh"
#include <QDir>
#include <QMessageBox>
#include <QIcon>
#include <QToolBar>
#include <set>
#include <map>

using std::set;
using std::wstring;
using std::map;
using std::pair;

MainWindow::MainWindow(): 
  addTab( this ),
  cfg( Config::load() ),
  articleMaker( dictionaries, groupInstances ),
  articleNetMgr( this, dictionaries, articleMaker ),
  wordFinder( this ),
  initializing( 0 )
{
  ui.setupUi( this );

  // Make the toolbar
  navToolbar = addToolBar( tr( "Navigation" ) );
  navBack = navToolbar->addAction( QIcon( ":/icons/previous.png" ), tr( "Back" ) );
  navForward = navToolbar->addAction( QIcon( ":/icons/next.png" ), tr( "Forward" ) );

  connect( navBack, SIGNAL( activated() ),
           this, SLOT( backClicked() ) );
  connect( navForward, SIGNAL( activated() ),
           this, SLOT( forwardClicked() ) );

  addTab.setAutoRaise( true );
  addTab.setIcon( QIcon( ":/icons/addtab.png" ) );

  ui.tabWidget->clear();

  ui.tabWidget->setCornerWidget( &addTab, Qt::TopLeftCorner );
  //ui.tabWidget->setCornerWidget( &closeTab, Qt::TopRightCorner );

  ui.tabWidget->setMovable( true );
  ui.tabWidget->setDocumentMode( true );

  connect( &addTab, SIGNAL( clicked() ),
           this, SLOT( addNewTab() ) );

  connect( ui.tabWidget, SIGNAL( tabCloseRequested( int ) ),
           this, SLOT( tabCloseRequested( int ) ) );

  ui.tabWidget->setTabsClosable( true );

  connect( ui.sources, SIGNAL( activated() ),
           this, SLOT( editSources() ) );

  connect( ui.groups, SIGNAL( activated() ),
           this, SLOT( editGroups() ) );

  connect( ui.translateLine, SIGNAL( textChanged( QString const & ) ),
           this, SLOT( translateInputChanged( QString const & ) ) );

  connect( ui.wordList, SIGNAL( itemActivated( QListWidgetItem * ) ),
           this, SLOT( wordListItemActivated( QListWidgetItem * ) ) );

  connect( wordFinder.qobject(), SIGNAL( prefixMatchComplete( WordFinderResults ) ),
           this, SLOT( prefixMatchComplete( WordFinderResults ) ) );

  makeDictionaries();

  addNewTab();

  ui.translateLine->setFocus();
}

LoadDictionaries::LoadDictionaries( vector< string > const & allFiles_ ):
  allFiles( allFiles_ )
{
}

void LoadDictionaries::run()
{
  {
    Bgl::Format bglFormat;
  
    dictionaries = bglFormat.makeDictionaries( allFiles, Config::getIndexDir().toLocal8Bit().data(), *this );
  }

  {
    Stardict::Format stardictFormat;
  
    vector< sptr< Dictionary::Class > > stardictDictionaries =
      stardictFormat.makeDictionaries( allFiles, Config::getIndexDir().toLocal8Bit().data(), *this );
  
    dictionaries.insert( dictionaries.end(), stardictDictionaries.begin(),
                         stardictDictionaries.end() );
  }

  {
    Lsa::Format lsaFormat;

    vector< sptr< Dictionary::Class > > lsaDictionaries =
      lsaFormat.makeDictionaries( allFiles, Config::getIndexDir().toLocal8Bit().data(), *this );

    dictionaries.insert( dictionaries.end(), lsaDictionaries.begin(),
                         lsaDictionaries.end() );
  }

  {
    Dsl::Format dslFormat;

    vector< sptr< Dictionary::Class > > dslDictionaries =
      dslFormat.makeDictionaries( allFiles, Config::getIndexDir().toLocal8Bit().data(), *this );

    dictionaries.insert( dictionaries.end(), dslDictionaries.begin(),
                         dslDictionaries.end() );
  }
}

void LoadDictionaries::indexingDictionary( string const & dictionaryName ) throw()
{
  emit indexingDictionarySignal( QString::fromUtf8( dictionaryName.c_str() ) );
}

void MainWindow::makeDictionaries()
{
  {
    DictLock _;
    dictionaries.clear();
  }

  ::Initializing init( this );

  try
  {
    initializing = &init;

    // Traverse through known directories in search for the files

    vector< string > allFiles;

    for( Config::Paths::const_iterator i = cfg.paths.begin();
         i != cfg.paths.end(); ++i )
    {
      QDir dir( *i );

      QStringList entries = dir.entryList();

      for( QStringList::const_iterator i = entries.constBegin();
           i != entries.constEnd(); ++i )
        allFiles.push_back( QDir::toNativeSeparators( dir.filePath( *i ) ).toLocal8Bit().data() );
    }

    // Now start a thread to load all the dictionaries

    LoadDictionaries loadDicts( allFiles );

    connect( &loadDicts, SIGNAL( indexingDictionarySignal( QString ) ),
             this, SLOT( indexingDictionary( QString ) ) );

    QEventLoop localLoop;

    connect( &loadDicts, SIGNAL( finished() ),
             &localLoop, SLOT( quit() ) );

    loadDicts.start();

    localLoop.exec();

    loadDicts.wait();

    {
      DictLock _;

      dictionaries = loadDicts.getDictionaries();
    }

    initializing = 0;

    // Remove any stale index files

    set< string > ids;

    for( unsigned x = dictionaries.size(); x--; )
      ids.insert( dictionaries[ x ]->getId() );

    QDir indexDir( Config::getIndexDir() );

    QStringList allIdxFiles = indexDir.entryList( QDir::Files );

    for( QStringList::const_iterator i = allIdxFiles.constBegin();
         i != allIdxFiles.constEnd(); ++i )
    {
      if ( ids.find( i->toLocal8Bit().data() ) == ids.end() &&
           i->size() == 32 )
        indexDir.remove( *i );
    }
  }
  catch( ... )
  {
    initializing = 0;
    throw;
  }

  updateStatusLine();
  updateGroupList();
  makeScanPopup();
}

void MainWindow::updateStatusLine()
{
  unsigned articleCount = 0, wordCount = 0;

  for( unsigned x = dictionaries.size(); x--; )
  {
    articleCount += dictionaries[ x ]->getArticleCount();
    wordCount += dictionaries[ x ]->getWordCount();
  }

  statusBar()->showMessage( tr( "%1 dictionaries, %2 articles, %3 words" ).
                              arg( dictionaries.size() ).arg( articleCount ).
                              arg( wordCount ) );
}

void MainWindow::updateGroupList()
{
  bool haveGroups = cfg.groups.size();

  ui.groupList->setVisible( haveGroups );

  ui.groupLabel->setText( haveGroups ? tr( "Look up in:" ) : tr( "Look up:" ) );

  {
    DictLock _;
  
    groupInstances.clear();
  
    for( unsigned x  = 0; x < cfg.groups.size(); ++x )
      groupInstances.push_back( Instances::Group( cfg.groups[ x ], dictionaries ) );
  }

  ui.groupList->fill( groupInstances );
}

void MainWindow::makeScanPopup()
{
  scanPopup.reset();

  scanPopup = new ScanPopup( 0, articleNetMgr, dictionaries, groupInstances );
}

vector< sptr< Dictionary::Class > > const & MainWindow::getActiveDicts()
{
  if ( cfg.groups.empty() )
    return dictionaries;

  int current = ui.groupList->currentIndex();

  if ( current < 0 || current >= (int) groupInstances.size() )
  {
    // This shouldn't ever happen
    return dictionaries;
  }

  return groupInstances[ current ].dictionaries;
}

void MainWindow::indexingDictionary( QString dictionaryName )
{
  if ( initializing )
    initializing->indexing( dictionaryName );
}

void MainWindow::addNewTab()
{
  ArticleView * view = new ArticleView( this, articleNetMgr, groupInstances,
                                        false );

  connect( view, SIGNAL( titleChanged(  ArticleView *, QString const & ) ),
           this, SLOT( titleChanged(  ArticleView *, QString const & ) ) );

  connect( view, SIGNAL( iconChanged( ArticleView *, QIcon const & ) ),
           this, SLOT( iconChanged( ArticleView *, QIcon const & ) ) );

  ui.tabWidget->addTab( view, tr( "(untitled)" ) );

  ui.tabWidget->setCurrentIndex( ui.tabWidget->count() - 1 );
}

void MainWindow::tabCloseRequested( int x )
{
  if ( ui.tabWidget->count() < 2 )
    return; // We should always have at least one open tab

  QWidget * w = ui.tabWidget->widget( x );

  ui.tabWidget->removeTab( x );

  delete w;
}

void MainWindow::backClicked()
{
  printf( "Back\n" );

  ArticleView & view =
    dynamic_cast< ArticleView & >( *( ui.tabWidget->currentWidget() ) );

  view.back();
}

void MainWindow::forwardClicked()
{
  printf( "Forward\n" );

  ArticleView & view =
    dynamic_cast< ArticleView & >( *( ui.tabWidget->currentWidget() ) );

  view.forward();
}

void MainWindow::titleChanged( ArticleView * view, QString const & title )
{
  ui.tabWidget->setTabText( ui.tabWidget->indexOf( view ), title );
}

void MainWindow::iconChanged( ArticleView * view, QIcon const & icon )
{
  ui.tabWidget->setTabIcon( ui.tabWidget->indexOf( view ), icon );
}

void MainWindow::editSources()
{
  Sources src( this, cfg.paths );

  src.show();

  if ( src.exec() == QDialog::Accepted )
  {
    cfg.paths = src.getPaths();

    makeDictionaries();

    Config::save( cfg );
  }
}

void MainWindow::editGroups()
{
  {
    // We lock all dictionaries during the entire group editing process, since
    // the dictionaries might get queried for various infos there
    DictLock _;
  
    Groups groups( this, dictionaries, cfg.groups );
  
    groups.show();

    if ( groups.exec() == QDialog::Accepted )
    {
      cfg.groups = groups.getGroups();

      Config::save( cfg );
    }
    else
      return;
  }

  updateGroupList();
  makeScanPopup();
}

void MainWindow::translateInputChanged( QString const & newValue )
{
  QString req = newValue.trimmed();

  if ( !req.size() )
  {
    // An empty request always results in an empty result
    prefixMatchComplete( WordFinderResults( req, &getActiveDicts() ) );

    return;
  }

  ui.wordList->setCursor( Qt::WaitCursor );

  wordFinder.prefixMatch( req, &getActiveDicts() );
}

void MainWindow::prefixMatchComplete( WordFinderResults r )
{
  if ( r.requestStr != ui.translateLine->text().trimmed() ||
      r.requestDicts != &getActiveDicts() )
  {
    // Those results are already irrelevant, ignore the result
    return;
  }

  ui.wordList->setUpdatesEnabled( false );

  for( unsigned x = 0; x < r.results.size(); ++x )
  {
    QListWidgetItem * i = ui.wordList->item( x );

    if ( !i )
      ui.wordList->addItem( r.results[ x ] );
    else
    if ( i->text() != r.results[ x ] )
      i->setText( r.results[ x ] );
  }

  while ( ui.wordList->count() > (int) r.results.size() )
  {
    // Chop off any extra items that were there
    QListWidgetItem * i = ui.wordList->takeItem( ui.wordList->count() - 1 );

    if ( i )
      delete i;
    else
      break;
  }

  if ( ui.wordList->count() )
    ui.wordList->scrollToItem( ui.wordList->item( 0 ), QAbstractItemView::PositionAtTop );

  ui.wordList->setUpdatesEnabled( true );
  ui.wordList->unsetCursor();
}

void MainWindow::wordListItemActivated( QListWidgetItem * item )
{
  printf( "act: %s\n", item->text().toLocal8Bit().data() );

  showTranslationFor( item->text() );
}

void MainWindow::showTranslationFor( QString const & inWord )
{
  ArticleView & view =
    dynamic_cast< ArticleView & >( *( ui.tabWidget->currentWidget() ) );

  view.showDefinition( inWord, cfg.groups.empty() ? "" :
                        groupInstances[ ui.groupList->currentIndex() ].name );

  #if 0
  QUrl req;

  req.setScheme( "gdlookup" );
  req.setHost( "localhost" );
  req.addQueryItem( "word", inWord );
  req.addQueryItem( "group",
                    cfg.groups.empty() ? "" :
                      groupInstances[ ui.groupList->currentIndex() ].name );

  ui.definition->load( req );

  return;
#endif

  #if 0
  wstring word = inWord.trimmed().toStdWString();

  // Where to look?

  vector< sptr< Dictionary::Class > > const & activeDicts = getActiveDicts();

  // Accumulate main forms

  vector< wstring > alts;

  {
    set< wstring > altsSet;
  
    for( unsigned x = 0; x < activeDicts.size(); ++x )
    {
      vector< wstring > found = activeDicts[ x ]->findHeadwordsForSynonym( word );
  
      altsSet.insert( found.begin(), found.end() );
    }

    alts.insert( alts.begin(), altsSet.begin(), altsSet.end() );
  }

  for( unsigned x = 0; x < alts.size(); ++x )
  {
    printf( "Alt: %ls\n", alts[ x ].c_str() );
  }


  string result =
    "<html><head>"
    "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">";

  QFile cssFile( Config::getUserCssFileName() );

  if ( cssFile.open( QFile::ReadOnly ) )
  {
    result += "<style type=\"text/css\">\n";
    result += cssFile.readAll().data();
    result += "</style>\n";
  }

  result += "</head><body>";

  for( unsigned x = 0; x < activeDicts.size(); ++x )
  {
    try
    {
      string body = activeDicts[ x ]->getArticle( word, alts );

      printf( "From %s: %s\n", activeDicts[ x ]->getName().c_str(), body.c_str() );

      result += "<div class=\"gddictname\">From " + activeDicts[ x ]->getName() + "</div>" + body;
    }
    catch( Dictionary::exNoSuchWord & )
    {
      continue;
    }
  }

  result += "</body></html>";

  ArticleMaker am( dictionaries, groupInstances );

  string result = am.makeDefinitionFor( inWord, "En" );

  ui.definition->setContent( result.c_str(), QString() );

  #endif

  //ui.tabWidget->setTabText( ui.tabWidget->indexOf(ui.tab), inWord.trimmed() );
}
