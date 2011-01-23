/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "nowplayingwidget.h"
#include "core/albumcoverloader.h"
#include "core/kittenloader.h"
#include "library/librarybackend.h"
#include "ui/coverfromurldialog.h"
#include "ui/iconloader.h"

#ifdef HAVE_LIBLASTFM
# include "core/albumcoverfetcher.h"
# include "ui/albumcovermanager.h"
# include "ui/albumcoversearcher.h"
#endif

#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMovie>
#include <QPainter>
#include <QPaintEvent>
#include <QSettings>
#include <QSignalMapper>
#include <QTextDocument>
#include <QTimeLine>
#include <QtDebug>

const char* NowPlayingWidget::kSettingsGroup = "NowPlayingWidget";

const char* NowPlayingWidget::kHypnotoadPath = ":/hypnotoad.gif";

// Space between the cover and the details in small mode
const int NowPlayingWidget::kPadding = 2;

// Width of the transparent to black gradient above and below the text in large
// mode
const int NowPlayingWidget::kGradientHead = 40;
const int NowPlayingWidget::kGradientTail = 20;

// Maximum height of the cover in large mode, and offset between the
// bottom of the cover and bottom of the widget
const int NowPlayingWidget::kMaxCoverSize = 260;
const int NowPlayingWidget::kBottomOffset = 0;

// Border for large mode
const int NowPlayingWidget::kTopBorder = 4;


NowPlayingWidget::NowPlayingWidget(QWidget *parent)
  : QWidget(parent),
    cover_from_url_dialog_(NULL),
    cover_loader_(new BackgroundThreadImplementation<AlbumCoverLoader, AlbumCoverLoader>(this)),
    kitten_loader_(NULL),
#ifdef HAVE_LIBLASTFM
    cover_searcher_(new AlbumCoverSearcher(QIcon(":/nocover.png"), this)),
    cover_fetcher_(new AlbumCoverFetcher(this)),
#endif
    backend_(NULL),
    mode_(SmallSongDetails),
    menu_(new QMenu(this)),
    above_statusbar_action_(NULL),
    visible_(false),
    small_ideal_height_(0),
    cover_height_(0),
    show_hide_animation_(new QTimeLine(500, this)),
    fade_animation_(new QTimeLine(1000, this)),
    load_cover_id_(0),
    details_(new QTextDocument(this)),
    previous_track_opacity_(0.0),
    hypnotoad_(NULL),
    aww_(false)
{
  // Load settings
  QSettings s;
  s.beginGroup(kSettingsGroup);
  mode_ = Mode(s.value("mode", SmallSongDetails).toInt());

  // Context menu
  QActionGroup* mode_group = new QActionGroup(this);
  QSignalMapper* mode_mapper = new QSignalMapper(this);
  connect(mode_mapper, SIGNAL(mapped(int)), SLOT(SetMode(int)));
  CreateModeAction(SmallSongDetails, tr("Small album cover"), mode_group, mode_mapper);
  CreateModeAction(LargeSongDetails, tr("Large album cover"), mode_group, mode_mapper);

  menu_->addActions(mode_group->actions());
  menu_->addSeparator();
  choose_cover_ = menu_->addAction(
        IconLoader::Load("document-open"), tr("Load cover from disk..."),
        this, SLOT(LoadCoverFromFile()));
  download_cover_ = menu_->addAction(
        IconLoader::Load("download"), tr("Load cover from URL..."),
        this, SLOT(LoadCoverFromURL()));
  search_for_cover_ = menu_->addAction(
        IconLoader::Load("find"), tr("Search for album covers..."),
        this, SLOT(SearchCover()));
  unset_cover_ = menu_->addAction(
        IconLoader::Load("list-remove"), tr("Unset cover"),
        this, SLOT(UnsetCover()));
  show_cover_ = menu_->addAction(
        IconLoader::Load("zoom-in"), tr("Show fullsize..."),
        this, SLOT(ZoomCover()));
  menu_->addSeparator();
  above_statusbar_action_ = menu_->addAction(tr("Show above status bar"));
  above_statusbar_action_->setCheckable(true);
  connect(above_statusbar_action_, SIGNAL(toggled(bool)), SLOT(ShowAboveStatusBar(bool)));
  above_statusbar_action_->setChecked(s.value("above_status_bar", false).toBool());

  // Animations
  connect(show_hide_animation_, SIGNAL(frameChanged(int)), SLOT(SetHeight(int)));
  setMaximumHeight(0);

  connect(fade_animation_, SIGNAL(valueChanged(qreal)), SLOT(FadePreviousTrack(qreal)));
  fade_animation_->setDirection(QTimeLine::Backward); // 1.0 -> 0.0

  // Start loading the cover loader thread
  cover_loader_->Start();
  connect(cover_loader_, SIGNAL(Initialised()), SLOT(CoverLoaderInitialised()));

#ifdef HAVE_LIBLASTFM
  cover_searcher_->Init(cover_fetcher_);
#endif
}

NowPlayingWidget::~NowPlayingWidget() {
  if(cover_from_url_dialog_) {
    delete cover_from_url_dialog_;
  }
}

void NowPlayingWidget::CreateModeAction(Mode mode, const QString &text, QActionGroup *group, QSignalMapper* mapper) {
  QAction* action = new QAction(text, group);
  action->setCheckable(true);
  mapper->setMapping(action, mode);
  connect(action, SIGNAL(triggered()), mapper, SLOT(map()));

  if (mode == mode_)
    action->setChecked(true);
}

void NowPlayingWidget::set_ideal_height(int height) {
  small_ideal_height_ = height;
  UpdateHeight(aww_
      ?  kitten_loader_->Worker().get()
      : cover_loader_->Worker().get());
}

QSize NowPlayingWidget::sizeHint() const {
  return QSize(cover_height_, total_height_);
}

void NowPlayingWidget::CoverLoaderInitialised() {
  BackgroundThread<AlbumCoverLoader>* loader =
      static_cast<BackgroundThread<AlbumCoverLoader>*>(sender());
  UpdateHeight(loader->Worker().get());
  loader->Worker()->SetPadOutputImage(true);
  connect(loader->Worker().get(), SIGNAL(ImageLoaded(quint64,QImage)),
          SLOT(AlbumArtLoaded(quint64,QImage)));
}

void NowPlayingWidget::UpdateHeight(AlbumCoverLoader* loader) {
  switch (mode_) {
  case SmallSongDetails:
    cover_height_ = small_ideal_height_;
    total_height_ = small_ideal_height_;
    break;

  case LargeSongDetails:
    cover_height_ = qMin(kMaxCoverSize, width());
    total_height_ = kTopBorder + cover_height_ + kBottomOffset;
    break;
  }

  // Update the animation settings and resize the widget now if we're visible
  show_hide_animation_->setFrameRange(0, total_height_);
  if (visible_ && show_hide_animation_->state() != QTimeLine::Running)
    setMaximumHeight(total_height_);

  // Tell the cover loader what size we want the images in
  loader->SetDesiredHeight(cover_height_);
  loader->SetDefaultOutputImage(QImage(":nocover.png"));

  // Re-fetch the current image
  load_cover_id_ = loader->LoadImageAsync(metadata_);

  // Tell Qt we've changed size
  updateGeometry();
}

void NowPlayingWidget::NowPlaying(const Song& metadata) {
  if (visible_) {
    // Cache the current pixmap so we can fade between them
    previous_track_ = QPixmap(size());
    previous_track_.fill(palette().background().color());
    previous_track_opacity_ = 1.0;
    QPainter p(&previous_track_);
    DrawContents(&p);
    p.end();
  }

  metadata_ = metadata;
  cover_ = QPixmap();

  // Loads the cover too.
  UpdateHeight(aww_
      ?  kitten_loader_->Worker().get()
      : cover_loader_->Worker().get());
  UpdateDetailsText();

  SetVisible(true);
  update();
}

void NowPlayingWidget::Stopped() {
  SetVisible(false);
}

void NowPlayingWidget::UpdateDetailsText() {
  QString html;

  switch (mode_) {
    case SmallSongDetails:
      details_->setTextWidth(-1);
      details_->setDefaultStyleSheet("");
      html += "<p>";
      break;

    case LargeSongDetails:
      details_->setTextWidth(cover_height_);
      details_->setDefaultStyleSheet("p {"
          "  font-size: small;"
          "  color: white;"
          "}");
      html += "<p align=center>";
      break;
  }

  // TODO: Make this configurable
  html += QString("<i>%1</i><br/>%2<br/>%3").arg(
      Qt::escape(metadata_.PrettyTitle()), Qt::escape(metadata_.artist()),
      Qt::escape(metadata_.album()));

  html += "</p>";
  details_->setHtml(html);
}

void NowPlayingWidget::AlbumArtLoaded(quint64 id, const QImage& image) {
  if (id != load_cover_id_)
    return;

  cover_ = QPixmap::fromImage(image);
  update();

  // Were we waiting for this cover to load before we started fading?
  if (!previous_track_.isNull()) {
    fade_animation_->start();
  }
}

void NowPlayingWidget::SetHeight(int height) {
  setMaximumHeight(height);
}

void NowPlayingWidget::SetVisible(bool visible) {
  if (visible == visible_)
    return;
  visible_ = visible;

  show_hide_animation_->setDirection(visible ? QTimeLine::Forward : QTimeLine::Backward);
  show_hide_animation_->start();
}

void NowPlayingWidget::paintEvent(QPaintEvent *e) {
  QPainter p(this);

  DrawContents(&p);

  // Draw the previous track's image if we're fading
  if (!previous_track_.isNull()) {
    p.setOpacity(previous_track_opacity_);
    p.drawPixmap(0, 0, previous_track_);
  }
}

void NowPlayingWidget::DrawContents(QPainter *p) {
  switch (mode_) {
  case SmallSongDetails:
    if (hypnotoad_) {
      p->drawPixmap(0, 0, small_ideal_height_, small_ideal_height_, hypnotoad_->currentPixmap());
    } else {
      // Draw the cover
      p->drawPixmap(0, 0, small_ideal_height_, small_ideal_height_, cover_);
    }

    // Draw the details
    p->translate(small_ideal_height_ + kPadding, 0);
    details_->drawContents(p);
    p->translate(-small_ideal_height_ - kPadding, 0);
    break;

  case LargeSongDetails:
    const int total_size = qMin(kMaxCoverSize, width());
    const int x_offset = (width() - cover_height_) / 2;

    // Draw the black background
    p->fillRect(QRect(0, kTopBorder, width(), height() - kTopBorder), Qt::black);

    // Draw the cover
    if (hypnotoad_) {
      p->drawPixmap(x_offset, kTopBorder, total_size, total_size, hypnotoad_->currentPixmap());
    } else {
      p->drawPixmap(x_offset, kTopBorder, total_size, total_size, cover_);
    }

    // Work out how high the text is going to be
    const int text_height = details_->size().height();
    const int gradient_mid = height() - qMax(text_height, kBottomOffset);

    // Draw the black fade
    QLinearGradient gradient(0, gradient_mid - kGradientHead,
                             0, gradient_mid + kGradientTail);
    gradient.setColorAt(0, QColor(0, 0, 0, 0));
    gradient.setColorAt(1, QColor(0, 0, 0, 255));

    p->fillRect(0, gradient_mid - kGradientHead,
                width(), height() - (gradient_mid - kGradientHead), gradient);

    // Draw the text on top
    p->translate(x_offset, height() - text_height);
    details_->drawContents(p);
    p->translate(-x_offset, -height() + text_height);
    break;
  }
}

void NowPlayingWidget::FadePreviousTrack(qreal value) {
  previous_track_opacity_ = value;
  if (qFuzzyCompare(previous_track_opacity_, 0.0)) {
    previous_track_ = QPixmap();
  }

  update();
}

void NowPlayingWidget::SetMode(int mode) {
  mode_ = Mode(mode);
  UpdateHeight(aww_
      ?  kitten_loader_->Worker().get()
      : cover_loader_->Worker().get());
  UpdateDetailsText();
  update();

  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("mode", mode_);
}

void NowPlayingWidget::resizeEvent(QResizeEvent* e) {
  if (visible_ && mode_ == LargeSongDetails && e->oldSize().width() != e->size().width()) {
    UpdateHeight(aww_
        ?  kitten_loader_->Worker().get()
        : cover_loader_->Worker().get());
    UpdateDetailsText();
  }
}

void NowPlayingWidget::contextMenuEvent(QContextMenuEvent* e) {
#ifndef HAVE_LIBLASTFM
  choose_cover_->setEnabled(false);
  search_for_cover_->setEnabled(false);
#endif

  const bool art_is_set =
      !metadata_.art_manual().isEmpty() &&
      metadata_.art_manual() != AlbumCoverLoader::kManuallyUnsetCover;

  unset_cover_->setEnabled(art_is_set);
  show_cover_->setEnabled(art_is_set);

  menu_->popup(mapToGlobal(e->pos()));
}

void NowPlayingWidget::ShowAboveStatusBar(bool above) {
  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("above_status_bar", above);

  emit ShowAboveStatusBarChanged(above);
}

bool NowPlayingWidget::show_above_status_bar() const {
  return above_statusbar_action_->isChecked();
}

void NowPlayingWidget::AllHail(bool hypnotoad) {
  if (hypnotoad) {
    hypnotoad_ = new QMovie(kHypnotoadPath, QByteArray(), this);
    connect(hypnotoad_, SIGNAL(updated(const QRect&)), SLOT(update()));
    hypnotoad_->start();
    update();
  } else {
    delete hypnotoad_;
    hypnotoad_ = NULL;
    update();
  }
}

void NowPlayingWidget::EnableKittens(bool aww) {
  if (!kitten_loader_ && aww) {
    kitten_loader_ = new BackgroundThreadImplementation<AlbumCoverLoader, KittenLoader>(this);
    kitten_loader_->Start();
    connect(kitten_loader_, SIGNAL(Initialised()), SLOT(CoverLoaderInitialised()));
  } else if (aww) {
    NowPlaying(metadata_);
  }

  aww_ = aww;
}

void NowPlayingWidget::LoadCoverFromFile() {
#ifdef HAVE_LIBLASTFM
  // Figure out the initial path.  Logic copied from
  // AlbumCoverManager::InitialPathForOpenCoverDialog
  QString dir;
  if (!metadata_.art_automatic().isEmpty() && metadata_.art_automatic() != AlbumCoverLoader::kEmbeddedCover) {
    dir = metadata_.art_automatic();
  } else {
    dir = metadata_.filename().section('/', 0, -1);
  }

  QString cover = QFileDialog::getOpenFileName(
      this, tr("Choose manual cover"), dir,
      tr(AlbumCoverManager::kImageFileFilter) + ";;" + tr(AlbumCoverManager::kAllFilesFilter));
  if (cover.isNull())
    return;

  // Can we load the image?
  QImage image(cover);
  if (image.isNull())
    return;

  // Update database
  SetAlbumArt(cover);
#endif
}

void NowPlayingWidget::LoadCoverFromURL() {
  if(!cover_from_url_dialog_) {
    cover_from_url_dialog_ = new CoverFromURLDialog(this);
  }

  QImage image = cover_from_url_dialog_->Exec();
  if (image.isNull())
    return;

  SetAlbumArt(AlbumCoverManager::SaveCoverInCache(
      metadata_.artist(), metadata_.album(), image));
}

void NowPlayingWidget::SearchCover() {
#ifdef HAVE_LIBLASTFM
  // Get something sensible to stick in the search box
  QString query = metadata_.artist();
  if (!query.isEmpty())
    query += " ";
  query += metadata_.album();

  QImage image = cover_searcher_->Exec(query);
  if (image.isNull())
    return;

  SetAlbumArt(AlbumCoverManager::SaveCoverInCache(
      metadata_.artist(), metadata_.album(), image));
#endif
}

void NowPlayingWidget::UnsetCover() {
  SetAlbumArt(AlbumCoverLoader::kManuallyUnsetCover);
}

void NowPlayingWidget::ZoomCover() {
  QDialog* dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose, true);
  dialog->setWindowTitle(metadata_.title());

  QLabel* label = new QLabel(dialog);
  label->setPixmap(AlbumCoverLoader::TryLoadPixmap(
      metadata_.art_automatic(), metadata_.art_manual(), metadata_.filename()));

  dialog->resize(label->pixmap()->size());
  dialog->show();
}

void NowPlayingWidget::SetAlbumArt(const QString& path) {
  metadata_.set_art_manual(path);
  backend_->UpdateManualAlbumArtAsync(metadata_.artist(), metadata_.album(), path);
  NowPlaying(metadata_);
}

void NowPlayingWidget::SetLibraryBackend(LibraryBackend* backend) {
  backend_ = backend;
}
