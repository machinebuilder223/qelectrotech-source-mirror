/*
	Copyright 2006-2012 Xavier Guerrin
	This file is part of QElectroTech.
	
	QElectroTech is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
	
	QElectroTech is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with QElectroTech.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "elementspanel.h"
#include "qetapp.h"
#include "qetproject.h"
#include "diagram.h"
#include "elementscategory.h"
#include "elementscollectioncache.h"
#include "customelement.h"
#include "fileelementscollection.h"
#include "fileelementdefinition.h"
#include "qeticons.h"
#include "templatescollection.h"

/**
	This class implements a thread reloading the following elements
	collections:
	  * the common collection
	  * the custom collection
	  * the embedded collection of each project listed in the projects_
	attribute.
*/
class ReloadCollectionThread : public QThread {
	public:
	void run();
	/// list of projects whose embedded collection should be reloaded.
	QList<QETProject *> projects_;
};

/**
	Reloads collections.
*/
void ReloadCollectionThread::run() {
	QETApp::commonElementsCollection() -> reload();
	QETApp::customElementsCollection() -> reload();
	
	// reloads collection of every project displayed in this panel
	foreach(QETProject *project, projects_) {
		if (ElementsCollection *project_collection = project -> embeddedCollection()) {
			project_collection -> reload();
		}
	}
	exit();
}

/*
	Lorsque le flag ENABLE_PANEL_DND_CHECKS est defini, le panel d'elements
	effectue des verifications lors des drag'n drop d'elements et categories.
	Par exemple, il verifie qu'une categorie cible est accessible en ecriture
	avant d'y autoriser le drop d'un element.
	Supprimer ce flag permet de tester le comportement des fonctions de gestion
	des items (copy, move, etc.).
*/
#define ENABLE_PANEL_DND_CHECKS

/*
	Largeur maximale, en pixels, de la pixmap accrochee au pointeur de la
	souris
*/
#define QET_MAX_DND_PIXMAP_WIDTH 500

/*
	Hauteur maximale, en pixels, de la pixmap accrochee au pointeur de la
	souris
*/
#define QET_MAX_DND_PIXMAP_HEIGHT 375

/**
	Constructeur
	@param parent Le QWidget parent du panel d'appareils
*/
ElementsPanel::ElementsPanel(QWidget *parent) :
	GenericPanel(parent),
	common_collection_item_(0),
	custom_collection_item_(0),
	first_reload_(true)
{
	// selection unique
	setSelectionMode(QAbstractItemView::SingleSelection);
	setColumnCount(1);
	setExpandsOnDoubleClick(true);
	
	// drag'n drop autorise
	setDragEnabled(true);
	setAcceptDrops(true);
	setDropIndicatorShown(true);
	setAutoExpandDelay(1000);
	
	// force du noir sur une alternance de blanc (comme le schema) et de gris
	// clair, avec du blanc sur bleu pas trop fonce pour la selection
	QPalette qp = palette();
	qp.setColor(QPalette::Text,            Qt::black);
	qp.setColor(QPalette::Base,            Qt::white);
	qp.setColor(QPalette::AlternateBase,   QColor("#e8e8e8"));
	qp.setColor(QPalette::Highlight,       QColor("#678db2"));
	qp.setColor(QPalette::HighlightedText, Qt::white);
	setPalette(qp);
	
	// we handle double click on items ourselves
	connect(
		this,
		SIGNAL(itemDoubleClicked(QTreeWidgetItem *, int)),
		this,
		SLOT(slot_doubleClick(QTreeWidgetItem *, int))
	);
	
	connect(this, SIGNAL(firstActivated()), this, SLOT(firstActivation()));
	
	// emet un signal au lieu de gerer son menu contextuel
	setContextMenuPolicy(Qt::CustomContextMenu);
	
	setElementsCache(QETApp::collectionCache());
}

/**
	Destructeur
*/
ElementsPanel::~ElementsPanel() {
}

/**
	@param qtwi Un QTreeWidgetItem
	@return true si qtwi represente un element, false sinon
*/
bool ElementsPanel::itemIsWritable(QTreeWidgetItem *qtwi) const {
	if (ElementsCollectionItem *qtwi_item = collectionItemForItem(qtwi)) {
		return(qtwi_item -> isWritable());
	}
	return(false);
}


/**
	@return true si l'item selectionne est accessible en ecriture, false sinon
*/
bool ElementsPanel::selectedItemIsWritable() const {
	if (ElementsCollectionItem *selected_item = selectedItem()) {
		return(selected_item -> isWritable());
	}
	return(false);
}

/**
	@return la collection, la categorie ou l'element selectionne(e)
*/
ElementsCollectionItem *ElementsPanel::selectedItem() const {
	ElementsLocation selected_location(selectedElementLocation());
	if (!selected_location.isNull()) {
		return(QETApp::collectionItem(selected_location));
	}
	return(0);
}

/**
	Gere l'entree d'un drag'n drop. L'evenement est accepte si les donnees
	fournies contiennent un type MIME representant une categorie ou un element
	QET.
	@param e QDragEnterEvent decrivant l'entree du drag'n drop
*/
void ElementsPanel::dragEnterEvent(QDragEnterEvent *e) {
	if (e -> mimeData() -> hasFormat("application/x-qet-category-uri")) {
		e -> acceptProposedAction();
	} else if (e -> mimeData() -> hasFormat("application/x-qet-element-uri")) {
		e -> acceptProposedAction();
	}
}

/**
	Gere le mouvement lors d'un drag'n drop
*/
void ElementsPanel::dragMoveEvent(QDragMoveEvent *e) {
	// scrolle lorsque le curseur est pres des bords
	int limit = 40;
	QScrollBar *scroll_bar = verticalScrollBar();
	if (e -> pos().y() < limit) {
		scroll_bar -> setValue(scroll_bar -> value() - 1);
	} else if (e -> pos().y() > height() - limit) {
		scroll_bar -> setValue(scroll_bar -> value() + 1);
	}
	
	QTreeWidget::dragMoveEvent(e);
	
	// recupere la categorie cible pour le deplacement / la copie
	ElementsCategory *target_category = categoryForPos(e -> pos());
	if (!target_category) {
		e -> ignore();
		return;
	}
	
	// recupere la source (categorie ou element) pour le deplacement / la copie
	ElementsLocation dropped_location = ElementsLocation::locationFromString(e -> mimeData() -> text());
	ElementsCollectionItem *source_item = QETApp::collectionItem(dropped_location, false);
	if (!source_item) {
		e -> ignore();
		return;
	}
	
#ifdef ENABLE_PANEL_DND_CHECKS
	// ne prend pas en consideration le drop d'un item sur lui-meme ou une categorie imbriquee
	if (
		source_item -> location() == target_category -> location() ||\
		target_category -> isChildOf(source_item)
	) {
		e -> ignore();
		return;
	}
	
	// s'assure que la categorie cible est accessible en ecriture
	if (!target_category -> isWritable()) {
		e -> ignore();
		return;
	}
#endif
	
	e -> accept();
	/// @todo mettre en valeur le lieu de depot 
}

/**
	Gere le depot lors d'un drag'n drop
	@param e QDropEvent decrivant le depot
*/
void ElementsPanel::dropEvent(QDropEvent *e) {
	// recupere la categorie cible pour le deplacement / la copie
	ElementsCategory *target_category = categoryForPos(e -> pos());
	if (!target_category) {
		e -> ignore();
		return;
	}
	
	// recupere la source (categorie ou element) pour le deplacement / la copie
	ElementsLocation dropped_location = ElementsLocation::locationFromString(e -> mimeData() -> text());
	ElementsCollectionItem *source_item = QETApp::collectionItem(dropped_location, false);
	if (!source_item) {
		e -> ignore();
		return;
	}
	
#ifdef ENABLE_PANEL_DND_CHECKS
	// ne prend pas en consideration le drop d'un item sur lui-meme ou une categorie imbriquee
	if (
		source_item -> location() == target_category -> location() ||\
		target_category -> isChildOf(source_item)
	) {
		e -> ignore();
		return;
	}
	
	// s'assure que la categorie cible est accessible en ecriture
	if (!target_category -> isWritable()) {
		e -> ignore();
		return;
	}
#endif
	
	e -> accept();
	emit(requestForMoveElements(source_item, target_category, e -> pos()));
}

/**
	Gere le debut des drag'n drop
	@param supportedActions Les actions supportees
*/
void ElementsPanel::startDrag(Qt::DropActions supportedActions) {
	Q_UNUSED(supportedActions);
	// recupere l'emplacement selectionne
	ElementsLocation element_location = selectedElementLocation();
	if (!element_location.isNull()) {
		startElementDrag(element_location);
		return;
	}
	
	TitleBlockTemplateLocation tbt_location = selectedTemplateLocation();
	if (tbt_location.isValid()) {
		startTitleBlockTemplateDrag(tbt_location);
		return;
	}
}

/**
	Handle the dragging of an element.
	@param location Location of the dragged element
*/
void ElementsPanel::startElementDrag(const ElementsLocation &location) {
	// recupere la selection
	ElementsCollectionItem *selected_item = QETApp::collectionItem(location);
	if (!selected_item) return;
	
	// objet QDrag pour realiser le drag'n drop
	QDrag *drag = new QDrag(this);
	
	// donnees qui seront transmises par le drag'n drop
	QString location_string(location.toString());
	QMimeData *mimeData = new QMimeData();
	mimeData -> setText(location_string);
	
	if (selected_item -> isCategory() || selected_item -> isCollection()) {
		mimeData -> setData("application/x-qet-category-uri", location_string.toAscii());
		drag -> setPixmap(QET::Icons::Folder.pixmap(22, 22));
	} else if (selected_item -> isElement()) {
		mimeData -> setData("application/x-qet-element-uri", location_string.toAscii());
		
		// element temporaire pour fournir un apercu
		int elmt_creation_state;
		Element *temp_elmt = new CustomElement(location, 0, 0, &elmt_creation_state);
		if (elmt_creation_state) {
			delete temp_elmt;
			return;
		}
		
		// accrochage d'une pixmap representant l'appareil au pointeur
		QPixmap elmt_pixmap(temp_elmt -> pixmap());
		QPoint elmt_hotspot(temp_elmt -> hotspot());
		
		// ajuste la pixmap si celle-ci est trop grande
		QPoint elmt_pixmap_size(elmt_pixmap.width(), elmt_pixmap.height());
		if (elmt_pixmap.width() > QET_MAX_DND_PIXMAP_WIDTH || elmt_pixmap.height() > QET_MAX_DND_PIXMAP_HEIGHT) {
			elmt_pixmap = elmt_pixmap.scaled(QET_MAX_DND_PIXMAP_WIDTH, QET_MAX_DND_PIXMAP_HEIGHT, Qt::KeepAspectRatio);
			elmt_hotspot = QPoint(
				elmt_hotspot.x() * elmt_pixmap.width() / elmt_pixmap_size.x(),
				elmt_hotspot.y() * elmt_pixmap.height() / elmt_pixmap_size.y()
			);
		}
		
		drag -> setPixmap(elmt_pixmap);
		drag -> setHotSpot(elmt_hotspot);
		
		// suppression de l'appareil temporaire
		delete temp_elmt;
	}
	
	// realisation du drag'n drop
	drag -> setMimeData(mimeData);
	drag -> start(Qt::MoveAction | Qt::CopyAction);
}

/**
	Handle the dragging of a title block template
	@param location Location of the dragged template.
*/
void ElementsPanel::startTitleBlockTemplateDrag(const TitleBlockTemplateLocation &location) {
	QString location_string = location.toString();
	
	QMimeData *mime_data = new QMimeData();
	mime_data -> setText(location_string);
	mime_data -> setData("application/x-qet-titleblock-uri", location_string.toAscii());
	
	QDrag *drag = new QDrag(this);
	drag -> setMimeData(mime_data);
	drag -> setPixmap(QET::Icons::TitleBlock.pixmap(22, 16));
	drag -> start(Qt::CopyAction);
}

void ElementsPanel::firstActivation() {
	QTimer::singleShot(250, this, SLOT(reload()));
}

/**
	Methode permettant d'ajouter un projet au panel d'elements.
	@param qtwi_parent QTreeWidgetItem parent sous lequel sera insere le projet
	@param project Projet a inserer dans le panel d'elements
	@return Le QTreeWidgetItem insere le plus haut
*/
QTreeWidgetItem *ElementsPanel::addProject(QETProject *project) {
	// create the QTreeWidgetItem representing the project
	QTreeWidgetItem *qtwi_project = GenericPanel::addProject(project, 0, GenericPanel::All);
	// the project will be inserted right before the common tb templates collection
	invisibleRootItem() -> insertChild(
		indexOfTopLevelItem(common_tbt_collection_item_),
		qtwi_project
	);
	qtwi_project -> setExpanded(true);
	itemForTemplatesCollection(project -> embeddedTitleBlockTemplatesCollection()) -> setExpanded(true);
	
	return(qtwi_project);
}

/**
	Methode privee permettant d'ajouter une collection d'elements au panel d'elements
	@param qtwi_parent QTreeWidgetItem parent sous lequel sera insere la collection d'elements
	@param collection Collection a inserer dans le panel d'elements - si
	collection vaut 0, cette methode retourne 0.
	@param coll_name Nom a utiliser pour la collection
	@param icon Icone a utiliser pour l'affichage de la collection
	@return Le QTreeWidgetItem insere le plus haut
*/
QTreeWidgetItem *ElementsPanel::addCollection(ElementsCollection *collection) {
	PanelOptions options = GenericPanel::AddAllChild;
	options |= GenericPanel::DisplayElementsPreview;
	return(addElementsCollection(collection, invisibleRootItem(), options));
}

QTreeWidgetItem *ElementsPanel::updateTemplateItem(QTreeWidgetItem *tb_template_qtwi, const TitleBlockTemplateLocation &tb_template, PanelOptions options, bool freshly_created) {
	QTreeWidgetItem *item = GenericPanel::updateTemplateItem(tb_template_qtwi, tb_template, options, freshly_created);
	item -> setStatusTip(
		0,
		tr(
			"Cliquer-d\351posez ce mod\350le de cartouche sur un sch\351ma pour l'y appliquer.",
			"Tip displayed when selecting a title block template"
		)
	);
	item -> setWhatsThis(
		0,
		tr(
			"Ceci est un mod\350le de cartouche, qui peut \320tre appliqu\351 a un sch\351ma.",
			"\"What's this\" tip"
		)
	);
	return(item);
}

QTreeWidgetItem *ElementsPanel::updateElementsCategoryItem(QTreeWidgetItem *category_qtwi, ElementsCategory *category, PanelOptions options, bool freshly_created) {
	QTreeWidgetItem *item = GenericPanel::updateElementsCategoryItem(category_qtwi, category, options, freshly_created);
	emit(loadingProgressed(++ loading_progress_, -1));
	return(item);
}

QTreeWidgetItem *ElementsPanel::updateElementItem(QTreeWidgetItem *element_qtwi, ElementDefinition *element, PanelOptions options, bool freshly_created) {
	QTreeWidgetItem *item = GenericPanel::updateElementItem(element_qtwi, element, options, freshly_created);
	
	QString whats_this = tr("Ceci est un \351l\351ment que vous pouvez ins\351rer dans votre sch\351ma par cliquer-d\351placer");
	item -> setWhatsThis(0, whats_this);
	
	QString status_tip = tr(
		"Cliquer-d\351posez cet \351l\351ment sur le sch\351ma pour ins\351rer un \351l\351ment \253 %1 \273",
		"Tip displayed in the status bar when selecting an element"
	);
	item -> setStatusTip(0, status_tip.arg(item -> text(0)));
	
	item -> setFlags(Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsEnabled);
	
	emit(loadingProgressed(++ loading_progress_, -1));
	return(item);
}

/**
	Reloads the following collections:
	  * common collection
	  * custom collection
	  * collection of every project displayed in this panel
*/
void ElementsPanel::reloadCollections() {
	ReloadCollectionThread thread;
	thread.projects_ = projects_to_display_.values();
	thread.start();
	while(!thread.wait(50)) {
		QApplication::processEvents();
	}
}

/**
	@return the count of categories and elements within the following collections:
	  * common collection
	  * custom collection
	  * collection of every project displayed in this panel
*/
int ElementsPanel::elementsCollectionItemsCount() {
	int items_count = 0;
	items_count += QETApp::commonElementsCollection() -> count();
	items_count += QETApp::customElementsCollection() -> count();
	foreach(QETProject *project, projects_to_display_.values()) {
		if (ElementsCollection *project_collection = project -> embeddedCollection()) {
			items_count += project_collection -> count();
		}
	}
	return(items_count);
}

/**
	Recharge l'arbre des elements
	@param reload_collections true pour relire les collections depuis leurs sources (fichiers, projets...)
*/
void ElementsPanel::reload(bool reload_collections) {
	if (reload_collections) {
		emit(readingAboutToBegin());
		reloadCollections();
		emit(readingFinished());
	}
	
	QIcon system_icon(":/ico/16x16/qet.png");
	QIcon user_icon(":/ico/16x16/go-home.png");
	
	// estimates the number of categories and elements to load
	int items_count = elementsCollectionItemsCount();
	emit(loadingProgressed(loading_progress_ = 0, items_count));
	
	// load the common title block templates collection
	TitleBlockTemplatesCollection *common_tbt_collection = QETApp::commonTitleBlockTemplatesCollection();
	common_tbt_collection_item_ = addTemplatesCollection(common_tbt_collection, invisibleRootItem());
	common_tbt_collection_item_ -> setIcon(0, system_icon);
	if (first_reload_) common_tbt_collection_item_ -> setExpanded(true);
	
	// load the common elements collection
	if (QETApp::commonElementsCollection()->rootCategory()) {
		common_collection_item_ = addCollection(QETApp::commonElementsCollection());
		if (first_reload_) common_collection_item_ -> setExpanded(true);
	}
	
	// load the custom title block templates collection
	TitleBlockTemplatesCollection *custom_tbt_collection = QETApp::customTitleBlockTemplatesCollection();
	custom_tbt_collection_item_ = addTemplatesCollection(custom_tbt_collection, invisibleRootItem());
	custom_tbt_collection_item_ -> setIcon(0, user_icon);
	if (first_reload_) custom_tbt_collection_item_ -> setExpanded(true);
	
	// load the custom elements collection
	if (QETApp::customElementsCollection()->rootCategory()) {
		custom_collection_item_ = addCollection(QETApp::customElementsCollection());
		if (first_reload_) custom_collection_item_ -> setExpanded(true);
	}
	
	// add projects
	foreach(QETProject *project, projects_to_display_.values()) {
		addProject(project);
	}
	
	// the first time, expand the first level of collections
	if (first_reload_) first_reload_ = false;
}

/**
	Gere le double-clic sur un element.
	Si un double-clic sur un projet est effectue, le signal requestForProject
	est emis.
	Si un double-clic sur un schema est effectue, le signal requestForDiagram
	est emis.
	Si un double-clic sur une collection, une categorie ou un element est
	effectue, le signal requestForCollectionItem est emis.
	@param qtwi
*/
void ElementsPanel::slot_doubleClick(QTreeWidgetItem *qtwi, int) {
	int qtwi_type = qtwi -> type();
	if (qtwi_type == QET::Project) {
		QETProject *project = valueForItem<QETProject *>(qtwi);
		emit(requestForProject(project));
	} else if (qtwi_type == QET::Diagram) {
		Diagram *diagram = valueForItem<Diagram *>(qtwi);
		emit(requestForDiagram(diagram));
	} else if (qtwi_type & QET::ElementsCollectionItem) {
		ElementsLocation element = valueForItem<ElementsLocation>(qtwi);
		emit(requestForCollectionItem(element));
	} else if (qtwi_type == QET::TitleBlockTemplate) {
		TitleBlockTemplateLocation tbt = valueForItem<TitleBlockTemplateLocation>(qtwi);
		emit(requestForTitleBlockTemplate(tbt));
	}
}

/**
	@param qtwi Un QTreeWidgetItem
	@return L'ElementsCollectionItem represente par qtwi, ou 0 si qtwi ne
	represente pas un ElementsCollectionItem
*/
ElementsCollectionItem *ElementsPanel::collectionItemForItem(QTreeWidgetItem *qtwi) const {
	if (qtwi && qtwi -> type() & QET::ElementsCollectionItem) {
		ElementsLocation item_location = elementLocationForItem(qtwi);
		return(QETApp::collectionItem(item_location));
	}
	return(0);
}

/**
	Cette methode permet d'acceder a la categorie correspondant a un item donne.
	Si cet item represente une collection, c'est sa categorie racine qui est renvoyee.
	Si cet item represente une categorie, c'est cette categorie qui est renvoyee.
	Si cet item represente un element, c'est sa categorie parente qui est renvoyee.
	@param qtwi un QTreeWidgetItem
	@return la categorie correspondant au QTreeWidgetItem qtwi, ou 0 s'il n'y a
	aucune categorie correspondante.
*/
ElementsCategory *ElementsPanel::categoryForItem(QTreeWidgetItem *qtwi) {
	if (!qtwi) return(0);
	
	// Recupere le CollectionItem associe a cet item
	ElementsCollectionItem *collection_item = collectionItemForItem(qtwi);
	if (!collection_item) return(0);
	
	// recupere la categorie cible pour le deplacement
	return(collection_item -> toCategory());
}

/**
	@param pos Position dans l'arborescence
	@return La categorie situee sous la position pos, ou 0 s'il n'y a aucune
	categorie correspondante.
	@see categoryForItem
*/
ElementsCategory *ElementsPanel::categoryForPos(const QPoint &pos) {
	// Accede a l'item sous la position
	QTreeWidgetItem *pos_qtwi = itemAt(pos);
	if (!pos_qtwi) {
		return(0);
	}
	
	return(categoryForItem(pos_qtwi));
}

/**
	Hide items that do not match the provided string, ensure others are visible
	along with their parent hierarchy. When ending the filtering, restore the tree
	as it was before the filtering (except the current item) and scroll to the
	currently selected item.
	@param m String to be matched
	@param filtering whether to begin/apply/end the filtering
	@see QET::Filtering
*/
void ElementsPanel::filter(const QString &m, QET::Filtering filtering) {
	QList<QTreeWidgetItem *> items = findItems("*", Qt::MatchRecursive | Qt::MatchWildcard);
	const int expanded_role = 42; // magic number? So you consider Douglas Adams wrote about magic?
	
	if (filtering == QET::BeginFilter) {
		foreach (QTreeWidgetItem *item, items) {
			item -> setData(0, expanded_role, item -> isExpanded());
		}
	}
	
	if (filtering != QET::EndFilter) {
		// repere les items correspondant au filtre
		QList<QTreeWidgetItem *> matching_items;
		foreach (QTreeWidgetItem *item, items) {
			bool item_matches = item -> text(0).contains(m, Qt::CaseInsensitive);
			if (item_matches) matching_items << item;
			item -> setHidden(!item_matches);
		}
		ensureHierarchyIsVisible(matching_items);
	} else { // filtering == QET::EndFilter
		QTreeWidgetItem *current_item = currentItem();
		
		// restore the tree as it was before the filtering
		foreach (QTreeWidgetItem *qtwi, items) {
			qtwi -> setHidden(false);
			qtwi -> setExpanded(qtwi -> data(0, expanded_role).toBool());
		}
		
		// avoid hiding the currently selected item
		if (current_item) {
			ensureHierarchyIsVisible(QList<QTreeWidgetItem *>() << current_item);
			scrollToItem(current_item);
		}
	}
}

/**
	Rajoute un projet au panel d'elements
	@param project Projet ouvert a rajouter au panel
*/
void ElementsPanel::projectWasOpened(QETProject *project) {
	projects_to_display_ << project;
	addProject(project);
}

/**
	Enleve un projet du panel d'elements
	@param project Projet a enlever du panel
*/
void ElementsPanel::projectWasClosed(QETProject *project) {
	if (QTreeWidgetItem *item_to_remove = itemForProject(project)) {
		GenericPanel::deleteItem(item_to_remove);
		projects_to_display_.remove(project);
	}
}

/**
	Affiche un element etant donne son emplacement
	@param location Emplacement de l'element a afficher
*/
bool ElementsPanel::scrollToElement(const ElementsLocation &location) {
	// recherche l'element dans le panel
	QTreeWidgetItem *item = itemForElementsLocation(location);
	if (!item) return(false);
	
	// s'assure que l'item ne soit pas filtre
	item -> setHidden(false);
	setCurrentItem(item);
	ensureHierarchyIsVisible(QList<QTreeWidgetItem *>() << item);
	scrollToItem(item);
	return(true);
}

/**
	@param items une liste de QTreeWidgetItem pour lesquels il faut s'assurer
	que eux et leurs parents sont visibles
*/
void ElementsPanel::ensureHierarchyIsVisible(QList<QTreeWidgetItem *> items) {
	// remonte l'arborescence pour lister les categories contenant les elements filtres
	QSet<QTreeWidgetItem *> parent_items;
	foreach(QTreeWidgetItem *item, items) {
		for (QTreeWidgetItem *parent_qtwi = item -> parent() ; parent_qtwi ; parent_qtwi = parent_qtwi -> parent()) {
			parent_items << parent_qtwi;
		}
	}
	
	// etend les parents
	foreach(QTreeWidgetItem *parent_qtwi, parent_items) {
		if (!parent_qtwi -> isExpanded()) parent_qtwi -> setExpanded(true);
	}
	
	// affiche les parents
	foreach(QTreeWidgetItem *parent_qtwi, parent_items) {
		if (parent_qtwi -> isHidden()) parent_qtwi -> setHidden(false);
	}
}
