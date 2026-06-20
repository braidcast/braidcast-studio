#include "CanvasDock.hpp"

#include <dialogs/CanvasEditorDialog.hpp>
#include <dialogs/NameDialog.hpp>
#include <dialogs/OBSBasicSourceSelect.hpp>
#include <utility/CanvasDefinition.hpp>
#include <utility/CanvasManager.hpp>
#include <utility/MultistreamOutput.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSBasicPreview.hpp>
#include <widgets/OBSQTDisplay.hpp>

#include <qt-wrappers.hpp>

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>

#include "moc_CanvasDock.cpp"

namespace {
constexpr uint32_t kPreviewBackground = 0x000000;
} // namespace

CanvasDock::CanvasDock(OBSBasic *main_, OBSCanvas canvas_, QWidget *parent)
	: OBSDock(parent),
	  main(main_),
	  canvas(canvas_)
{
	const char *uuid = obs_canvas_get_uuid(canvas);
	canvasUuid = uuid ? uuid : "";

	setObjectName(QStringLiteral("canvasDock_") + QString::fromStdString(canvasUuid));

	const char *name = obs_canvas_get_name(canvas);
	QString title = QString::fromUtf8(name ? name : "");
	struct obs_video_info ovi;
	if (obs_canvas_get_video_info(canvas, &ovi)) {
		title += QString(" (%1x%2)").arg(ovi.base_width).arg(ovi.base_height);
	}
	setWindowTitle(title);

	auto sectionHeader = [](const QString &text) {
		QLabel *label = new QLabel(text.toUpper());
		/* Plain labels read as body text; give them a tinted bar, weight and
		 * letter-spacing so each section reads as a header (palette-based so it
		 * holds on both light and dark themes). */
		label->setStyleSheet("background-color: palette(button); color: palette(text); font-weight: 700;"
				     " letter-spacing: 1px; padding: 5px 8px; border-radius: 3px;");
		return label;
	};
	auto iconButton = [](const char *cls, const QString &tip) {
		QPushButton *button = new QPushButton();
		button->setProperty("class", cls);
		button->setFlat(true);
		button->setToolTip(tip);
		return button;
	};
	auto sectionToolbar = []() {
		QHBoxLayout *toolbar = new QHBoxLayout();
		toolbar->setContentsMargins(0, 0, 0, 0);
		toolbar->setSpacing(0);
		return toolbar;
	};

	sceneList = new QListWidget();
	sceneList->setContextMenuPolicy(Qt::CustomContextMenu);

	sourceList = new QListWidget();
	sourceList->setSelectionMode(QAbstractItemView::SingleSelection);

	QWidget *sceneContainer = new QWidget();
	QVBoxLayout *sceneLayout = new QVBoxLayout(sceneContainer);
	sceneLayout->setContentsMargins(6, 6, 6, 6);
	sceneLayout->setSpacing(4);

	sceneLayout->addWidget(sectionHeader(QTStr("Basic.Main.Scenes")));
	sceneLayout->addWidget(sceneList, 1);

	QPushButton *addSceneButton = iconButton("icon-plus", QTStr("AddScene"));
	QPushButton *removeSceneButton = iconButton("icon-trash", QTStr("RemoveScene"));
	QHBoxLayout *sceneToolbar = sectionToolbar();
	sceneToolbar->addWidget(addSceneButton);
	sceneToolbar->addWidget(removeSceneButton);
	sceneToolbar->addStretch();
	sceneLayout->addLayout(sceneToolbar);

	QFrame *divider = new QFrame();
	divider->setFixedHeight(1);
	divider->setStyleSheet("background-color: palette(mid);");
	sceneLayout->addWidget(divider);

	sceneLayout->addWidget(sectionHeader(QTStr("Basic.Main.Sources")));
	sceneLayout->addWidget(sourceList, 1);

	QPushButton *addSourceButton = iconButton("icon-plus", QTStr("AddSource"));
	QPushButton *removeSourceButton = iconButton("icon-trash", QTStr("RemoveSource"));
	QPushButton *sourcePropsButton = iconButton("icon-gear", QTStr("Properties"));
	QPushButton *sourceUpButton = iconButton("icon-up", QTStr("MoveUp"));
	QPushButton *sourceDownButton = iconButton("icon-down", QTStr("MoveDown"));
	QHBoxLayout *sourceToolbar = sectionToolbar();
	sourceToolbar->addWidget(addSourceButton);
	sourceToolbar->addWidget(removeSourceButton);
	sourceToolbar->addWidget(sourcePropsButton);
	sourceToolbar->addWidget(sourceUpButton);
	sourceToolbar->addWidget(sourceDownButton);
	sourceToolbar->addStretch();
	sceneLayout->addLayout(sourceToolbar);

	preview = new OBSBasicPreview(nullptr);
	preview->SetTargetCanvas(canvas);
	preview->Init();

	QWidget *placeholder = new QWidget();
	QVBoxLayout *placeholderLayout = new QVBoxLayout(placeholder);
	placeholderLayout->setAlignment(Qt::AlignCenter);
	QLabel *gateTitle = new QLabel(QTStr("Basic.CanvasDock.PreviewDisabled"));
	gateTitle->setAlignment(Qt::AlignCenter);
	gateTitle->setWordWrap(true);
	QLabel *gateHint = new QLabel(QTStr("Basic.CanvasDock.PreviewDisabled.Hint"));
	gateHint->setAlignment(Qt::AlignCenter);
	gateHint->setWordWrap(true);
	placeholderLayout->addWidget(gateTitle);
	placeholderLayout->addWidget(gateHint);

	previewStack = new QStackedWidget();
	previewStack->addWidget(preview);
	previewStack->addWidget(placeholder);

	splitter = new QSplitter();
	splitter->addWidget(sceneContainer);
	splitter->addWidget(previewStack);
	splitter->setStretchFactor(1, 1);

	QPushButton *editCanvasButton = iconButton("icon-gear", QTStr("Basic.CanvasDock.EditCanvas"));

	statusDot = new QLabel();
	statusDot->setFixedSize(12, 12);

	QWidget *footer = new QWidget();
	QHBoxLayout *footerLayout = new QHBoxLayout(footer);
	footerLayout->setContentsMargins(8, 5, 8, 5);
	footerLayout->setSpacing(4);
	footerLayout->addWidget(editCanvasButton);
	footerLayout->addStretch();
	footerLayout->addWidget(statusDot);

	QWidget *container = new QWidget();
	QVBoxLayout *containerLayout = new QVBoxLayout(container);
	containerLayout->setContentsMargins(0, 0, 0, 0);
	containerLayout->setSpacing(0);
	containerLayout->addWidget(splitter, 1);
	containerLayout->addWidget(footer);
	setWidget(container);

	connect(editCanvasButton, &QPushButton::clicked, this, &CanvasDock::EditCanvas);

	connect(addSceneButton, &QPushButton::clicked, this, &CanvasDock::AddScene);
	connect(removeSceneButton, &QPushButton::clicked, this, &CanvasDock::RemoveScene);
	connect(addSourceButton, &QPushButton::clicked, this, &CanvasDock::AddSource);
	connect(removeSourceButton, &QPushButton::clicked, this, &CanvasDock::RemoveSource);
	connect(sourcePropsButton, &QPushButton::clicked, this, &CanvasDock::OpenSourceProperties);
	connect(sourceUpButton, &QPushButton::clicked, this, [this]() { MoveSource(OBS_ORDER_MOVE_UP); });
	connect(sourceDownButton, &QPushButton::clicked, this, [this]() { MoveSource(OBS_ORDER_MOVE_DOWN); });
	connect(sceneList, &QListWidget::itemSelectionChanged, this, &CanvasDock::OnSceneSelected);
	connect(sceneList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		QMenu menu(this);
		menu.addAction(QTStr("Add"), this, &CanvasDock::AddScene);
		if (sceneList->currentItem()) {
			menu.addAction(QTStr("Rename"), this, &CanvasDock::RenameScene);
			menu.addAction(QTStr("Remove"), this, &CanvasDock::RemoveScene);
		}
		menu.exec(sceneList->mapToGlobal(pos));
	});
	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(preview->GetDisplay(), OBSBasicPreview::CanvasPreviewRender, preview);
		obs_display_set_background_color(preview->GetDisplay(), kPreviewBackground);
		/* The display is created async, possibly after the initial gate compute;
		 * re-apply so a gated canvas stops its GPU render rather than just
		 * early-returning out of the callback. */
		obs_display_set_enabled(preview->GetDisplay(), !previewGated);
	};
	connect(preview, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);

	/* Scene add/remove and current-scene changes reflect even when triggered
	 * outside this dock (scene-link sync, Settings edits). */
	signal_handler_t *handler = obs_canvas_get_signal_handler(canvas);
	sigs.emplace_back(handler, "source_add", SceneListChanged, this);
	sigs.emplace_back(handler, "source_remove", SceneListChanged, this);
	sigs.emplace_back(handler, "channel_change", ChannelChanged, this);

	connect(main, &OBSBasic::OutputBindingsChanged, this, &CanvasDock::UpdatePreviewGate);

	obs_frontend_add_event_callback(OnFrontendEvent, this);

	statusTimer = new QTimer(this);
	connect(statusTimer, &QTimer::timeout, this, &CanvasDock::UpdateStatusDot);
	statusTimer->start(1000);

	RefreshScenes();
	RefreshSources();
	ApplyOrientation();

	UpdatePreviewGate();
	UpdateStatusDot();
}

CanvasDock::~CanvasDock()
{
	/* The frontend event callback fires cross-thread with `this`; remove it
	 * before any other teardown so it can't reach a half-destroyed dock. */
	obs_frontend_remove_event_callback(OnFrontendEvent, this);

	/* Disconnect the canvas and per-scene item signals first: they fire on
	 * core/graphics threads with `this` as param, so leaving them live during
	 * teardown is a UAF. */
	sceneItemSigs.clear();
	sigs.clear();

	/* libobs blocks until the graphics thread is out of the callback, so the
	 * canvas ref is safe to drop after this returns. */
	obs_display_remove_draw_callback(preview->GetDisplay(), OBSBasicPreview::CanvasPreviewRender, preview);
}

void CanvasDock::SceneListChanged(void *data, calldata_t *)
{
	CanvasDock *window = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(window, "RefreshScenes", Qt::QueuedConnection);
}

void CanvasDock::ChannelChanged(void *data, calldata_t *params)
{
	long long channel = calldata_int(params, "channel");
	if (channel != 0) {
		return;
	}
	CanvasDock *window = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(window, "RefreshScenes", Qt::QueuedConnection);
	QMetaObject::invokeMethod(window, "RefreshSources", Qt::QueuedConnection);
}

void CanvasDock::SourceListChanged(void *data, calldata_t *)
{
	CanvasDock *window = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(window, "RefreshSources", Qt::QueuedConnection);
}

void CanvasDock::OnFrontendEvent(enum obs_frontend_event event, void *data)
{
	/* SCENE_CHANGED keeps the highlighted current row in sync after a link-driven
	 * switch; SCENE_LIST_CHANGED keeps the "follows" dropdowns listing the current
	 * set of global scenes. */
	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED && event != OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
		return;
	}
	CanvasDock *window = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(window, "RefreshScenes", Qt::QueuedConnection);
}

void CanvasDock::RefreshScenes()
{
	refreshing = true;
	sceneList->clear();

	OBSSource current = main->GetCanvasCurrentScene(canvas);
	const char *currentUuid = current ? obs_source_get_uuid(current) : nullptr;

	/* The global (main-canvas) scenes a canvas scene can be set to follow. */
	std::vector<std::pair<std::string, std::string>> mainScenes; // {uuid, name}
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *s = scenes.sources.array[i];
		const char *u = obs_source_get_uuid(s);
		const char *n = obs_source_get_name(s);
		mainScenes.emplace_back(u ? u : "", n ? n : "");
	}
	obs_frontend_source_list_free(&scenes);

	struct Ctx {
		CanvasDock *dock;
		QListWidget *list;
		const char *currentUuid;
		const std::vector<std::pair<std::string, std::string>> *mainScenes;
		int currentRow;
	} ctx{this, sceneList, currentUuid, &mainScenes, -1};

	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *scene) {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_source_get_name(scene);
			const char *uuid = obs_source_get_uuid(scene);
			std::string sUuid = uuid ? uuid : "";

			QWidget *row = new QWidget();
			QHBoxLayout *rowLayout = new QHBoxLayout(row);
			rowLayout->setContentsMargins(4, 2, 4, 2);
			rowLayout->setSpacing(4);

			QLabel *label = new QLabel(QString::fromUtf8(name ? name : ""));
			label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

			/* "Follows" picker: when the chosen global scene becomes the active
			 * program scene, this canvas switches to this scene (activation sync).
			 * Explicit, unlike binding to whatever scene happens to be live. */
			CanvasDock *dock = c->dock;
			CanvasSceneLink &links = dock->main->GetCanvasSceneLink();
			QComboBox *follow = new QComboBox();
			follow->setToolTip(QTStr("Basic.CanvasDock.LinkScene"));
			follow->setMaximumWidth(160);
			follow->addItem(QTStr("Basic.CanvasDock.LinkNone"), QString());
			int currentIdx = 0;
			for (const std::pair<std::string, std::string> &ms : *c->mainScenes) {
				follow->addItem(QString::fromUtf8(ms.second.c_str()), QString::fromStdString(ms.first));
				if (links.Resolve(ms.first, dock->canvasUuid) == sUuid) {
					currentIdx = follow->count() - 1;
				}
			}
			follow->setCurrentIndex(currentIdx);

			connect(follow, &QComboBox::activated, dock, [dock, sUuid, follow](int index) {
				std::string newMain = follow->itemData(index).toString().toStdString();
				CanvasSceneLink &l = dock->main->GetCanvasSceneLink();
				/* A canvas scene follows at most one global scene: clear any
				 * prior mapping to it before setting the new target. */
				l.UnsetByCanvasScene(dock->canvasUuid, sUuid);
				if (!newMain.empty()) {
					l.Set(newMain, dock->canvasUuid, sUuid);
				}
				dock->main->SaveProject();
				/* Rebuild deferred: this combo is the signal's sender, so don't
				 * delete it synchronously inside its own handler. */
				QMetaObject::invokeMethod(dock, "RefreshScenes", Qt::QueuedConnection);
			});

			rowLayout->addWidget(label);
			rowLayout->addWidget(follow);

			QListWidgetItem *item = new QListWidgetItem();
			QSize rowHint = row->sizeHint();
			rowHint.setHeight(std::max(rowHint.height(), follow->sizeHint().height()));
			item->setSizeHint(rowHint);
			item->setData(Qt::UserRole, QString::fromStdString(sUuid));
			c->list->addItem(item);
			c->list->setItemWidget(item, row);

			if (c->currentUuid && uuid && strcmp(c->currentUuid, uuid) == 0) {
				c->currentRow = c->list->row(item);
			}
			return true;
		},
		&ctx);

	if (ctx.currentRow >= 0) {
		sceneList->setCurrentRow(ctx.currentRow);
	}

	refreshing = false;
}

void CanvasDock::RefreshSources()
{
	sourceList->clear();

	/* Re-point the item signals at whatever scene is now current: the prior
	 * scene's subscriptions are stale once the dock switches scenes. */
	sceneItemSigs.clear();

	OBSSource current = main->GetCanvasCurrentScene(canvas);
	obs_scene_t *scene = obs_scene_from_source(current);
	if (!scene) {
		return;
	}

	struct Ctx {
		CanvasDock *dock;
		QListWidget *list;
	} ctx{this, sourceList};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *name = obs_source_get_name(source);

			QWidget *row = new QWidget();
			QHBoxLayout *rowLayout = new QHBoxLayout(row);
			rowLayout->setContentsMargins(4, 2, 4, 2);
			rowLayout->setSpacing(4);

			QLabel *label = new QLabel(QString::fromUtf8(name ? name : ""));
			label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			label->setEnabled(obs_sceneitem_visible(item));

			QCheckBox *vis = new QCheckBox();
			vis->setProperty("class", "checkbox-icon indicator-visibility");
			vis->setChecked(obs_sceneitem_visible(item));
			vis->setAccessibleName(QTStr("Basic.Main.Sources.Visibility"));
			vis->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

			QCheckBox *lock = new QCheckBox();
			lock->setProperty("class", "checkbox-icon indicator-lock");
			lock->setChecked(obs_sceneitem_locked(item));
			lock->setAccessibleName(QTStr("Basic.Main.Sources.Lock"));
			lock->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

			/* Capture a strong ref so a click can't reach a freed item if the
			 * row outlives it (e.g. a shared source removed from another canvas
			 * fires item_remove before the queued rebuild drops this row). */
			OBSSceneItem itemRef = item;
			connect(vis, &QAbstractButton::clicked, vis,
				[itemRef](bool checked) { obs_sceneitem_set_visible(itemRef, checked); });
			connect(lock, &QAbstractButton::clicked, lock,
				[itemRef](bool checked) { obs_sceneitem_set_locked(itemRef, checked); });

			rowLayout->addWidget(label);
			rowLayout->addWidget(vis);
			rowLayout->addWidget(lock);

			QListWidgetItem *listItem = new QListWidgetItem();
			/* sizeHint() on the not-yet-polished row underestimates and clips the
			 * label; floor the height to the font's natural height plus padding. */
			QSize rowHint = row->sizeHint();
			rowHint.setHeight(std::max(rowHint.height(), label->fontMetrics().height() + 10));
			listItem->setSizeHint(rowHint);
			/* Store the item id (not the pointer) so the toolbar re-resolves a
			 * live item after any rebuild rather than holding a stale pointer. */
			listItem->setData(Qt::UserRole, (qlonglong)obs_sceneitem_get_id(item));

			/* enum_items walks bottom-to-top (z-order); insert at the top so the
			 * topmost source lists first, matching the main Sources tree. */
			c->list->insertItem(0, listItem);
			c->list->setItemWidget(listItem, row);
			return true;
		},
		&ctx);

	signal_handler_t *sh = obs_source_get_signal_handler(current);
	sceneItemSigs.emplace_back(sh, "item_add", SourceListChanged, this);
	sceneItemSigs.emplace_back(sh, "item_remove", SourceListChanged, this);
	sceneItemSigs.emplace_back(sh, "item_visible", SourceListChanged, this);
	sceneItemSigs.emplace_back(sh, "item_locked", SourceListChanged, this);
	sceneItemSigs.emplace_back(sh, "reorder", SourceListChanged, this);
}

obs_sceneitem_t *CanvasDock::SelectedSceneItem()
{
	QListWidgetItem *item = sourceList->currentItem();
	if (!item) {
		return nullptr;
	}
	OBSSource current = main->GetCanvasCurrentScene(canvas);
	obs_scene_t *scene = obs_scene_from_source(current);
	if (!scene) {
		return nullptr;
	}
	return obs_scene_find_sceneitem_by_id(scene, item->data(Qt::UserRole).toLongLong());
}

void CanvasDock::AddSource()
{
	OBSSource current = main->GetCanvasCurrentScene(canvas);
	OBSScene scene = obs_scene_from_source(current);
	if (!scene) {
		return;
	}

	OBSBasicSourceSelect sourceSelect(main, main->undo_s, scene);
	sourceSelect.exec();
	/* createNew/addExisting fire item_add -> SourceListChanged rebuilds the list. */
}

void CanvasDock::RemoveSource()
{
	obs_sceneitem_t *item = SelectedSceneItem();
	if (!item) {
		return;
	}
	obs_sceneitem_remove(item);
	main->SaveProject();
}

void CanvasDock::OpenSourceProperties()
{
	obs_sceneitem_t *item = SelectedSceneItem();
	if (!item) {
		return;
	}
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (source) {
		main->CreatePropertiesWindow(source);
	}
}

void CanvasDock::MoveSource(enum obs_order_movement movement)
{
	obs_sceneitem_t *item = SelectedSceneItem();
	if (!item) {
		return;
	}
	obs_sceneitem_set_order(item, movement);
	main->SaveProject();
}

void CanvasDock::OnSceneSelected()
{
	if (refreshing) {
		return;
	}

	QListWidgetItem *item = sceneList->currentItem();
	if (!item) {
		return;
	}

	std::string uuid = item->data(Qt::UserRole).toString().toStdString();
	obs_source_t *scene = main->FindCanvasSceneByUuid(canvas, uuid);
	if (scene) {
		main->SetCanvasCurrentScene(canvas, scene);
	}
}

void CanvasDock::AddScene()
{
	std::string name;
	if (!NameDialog::AskForName(this, QTStr("Basic.Main.AddSceneDlg.Title"), QTStr("Basic.Main.AddSceneDlg.Text"),
				    name)) {
		return;
	}
	if (name.empty()) {
		return;
	}

	OBSSceneAutoRelease scene = obs_canvas_scene_create(canvas, name.c_str());
	if (scene) {
		main->SetCanvasCurrentScene(canvas, obs_scene_get_source(scene));
	}
	RefreshScenes();
}

void CanvasDock::RenameScene()
{
	QListWidgetItem *item = sceneList->currentItem();
	if (!item) {
		return;
	}

	std::string uuid = item->data(Qt::UserRole).toString().toStdString();
	/* Scene rows are widgets, not text items; seed the dialog from the source. */
	obs_source_t *existing = main->FindCanvasSceneByUuid(canvas, uuid);
	const char *existingName = existing ? obs_source_get_name(existing) : nullptr;
	std::string name = existingName ? existingName : "";
	if (!NameDialog::AskForName(this, QTStr("Basic.Main.AddSceneDlg.Title"), QTStr("Basic.Main.AddSceneDlg.Text"),
				    name)) {
		return;
	}
	if (name.empty()) {
		return;
	}

	/* Re-resolve after the modal: the event loop spun while it was open, so the
	 * scene found before it could have been removed in the meantime. */
	obs_source_t *scene = main->FindCanvasSceneByUuid(canvas, uuid);
	if (!scene) {
		return;
	}

	obs_source_set_name(scene, name.c_str());
	RefreshScenes();
}

void CanvasDock::RemoveScene()
{
	QListWidgetItem *item = sceneList->currentItem();
	if (!item) {
		return;
	}

	std::string uuid = item->data(Qt::UserRole).toString().toStdString();
	obs_source_t *scene = main->FindCanvasSceneByUuid(canvas, uuid);
	if (!scene) {
		return;
	}

	obs_source_remove(scene);
	RefreshScenes();
}

void CanvasDock::resizeEvent(QResizeEvent *event)
{
	OBSDock::resizeEvent(event);
	ApplyOrientation();
}

void CanvasDock::UpdatePreviewGate()
{
	const bool enabled = main->GetOutputBindings().AnyEnabledForCanvas(canvasUuid);
	previewGated = !enabled;
	previewStack->setCurrentIndex(enabled ? 0 : 1);
	if (preview && preview->GetDisplay()) {
		obs_display_set_enabled(preview->GetDisplay(), enabled);
	}
}

void CanvasDock::EditCanvas()
{
	CanvasDefinition *def = main->GetCanvasManager().Find(canvasUuid);
	if (!def) {
		return;
	}

	CanvasEditorDialog dlg(*def, main, this);
	if (dlg.exec() != QDialog::Accepted) {
		return;
	}

	/* Dock canvases are always non-default: mirror the non-default branch of
	 * OBSBasicSettings::ApplyCanvasEdit. */
	obs_video_info ovi = {};
	def->ToVideoInfo(ovi, &main->GetCanvasManager().Default());
	if (!obs_canvas_reset_video(static_cast<obs_canvas_t *>(canvas), &ovi)) {
		blog(LOG_WARNING, "Failed to apply canvas '%s' resolution %ux%u", def->name.c_str(), def->width,
		     def->height);
	}
	if (auto *ms = main->GetMultistreamOutput()) {
		ms->InvalidateCanvasEncoders(canvasUuid);
	}
	main->GetCanvasManager().Save();
}

void CanvasDock::UpdateStatusDot()
{
	if (!statusDot) {
		return;
	}

	MultistreamOutput::State state = MultistreamOutput::State::Idle;
	if (MultistreamOutput *engine = main->GetMultistreamOutput()) {
		/* Error > Live > Connecting > Idle across this canvas's enabled
		 * bindings; the highest-priority state colors the dot. */
		for (const MultistreamOutput::OutputStatus &status : engine->Statuses()) {
			if (status.canvasUuid != canvasUuid) {
				continue;
			}
			if (static_cast<int>(status.state) > static_cast<int>(state)) {
				state = status.state;
			}
		}
	}

	statusDot->setStyleSheet(
		QString("border-radius:6px; background:%1;").arg(MultistreamOutput::StateColor(state)));
}

void CanvasDock::ApplyOrientation()
{
	if (!splitter) {
		return;
	}

	QWidget *content = widget();
	if (!content) {
		return;
	}

	/* Orientation follows the dock's shape; the splitter handle stays
	 * user-draggable for sizing within a given orientation. */
	Qt::Orientation wanted = content->height() > content->width() ? Qt::Vertical : Qt::Horizontal;
	if (splitter->orientation() != wanted) {
		splitter->setOrientation(wanted);
	}
}
