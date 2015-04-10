/**
   @file
*/

#include "JointItem.h"
#include <cnoid/LeggedBodyHelper>
#include <cnoid/YAMLReader>
#include <cnoid/EigenArchive>
#include <cnoid/Archive>
#include <cnoid/ItemTreeView>
#include <cnoid/RootItem>
#include <cnoid/LazySignal>
#include <cnoid/LazyCaller>
#include <cnoid/MessageView>
#include <cnoid/ItemManager>
#include <cnoid/OptionManager>
#include <cnoid/MenuManager>
#include <cnoid/PutPropertyFunction>
#include <cnoid/JointPath>
#include <cnoid/BodyLoader>
#include <cnoid/BodyState>
#include <cnoid/SceneBody>
#include <cnoid/SceneShape>
#include "ModelEditDragger.h"
#include <cnoid/FileUtil>
#include <cnoid/MeshGenerator>
#include <boost/bind.hpp>
#include <boost/make_shared.hpp>
#include <bitset>
#include <deque>
#include <iostream>
#include <algorithm>
#include "gettext.h"

using namespace std;
using namespace cnoid;

namespace {

const char* axisNames[3] = { "x", "y", "z" };

const bool TRACE_FUNCTIONS = false;

inline double radian(double deg) { return (3.14159265358979 * deg / 180.0); }

}


namespace cnoid {

class JointItemImpl
{
public:
    JointItem* self;
    Link* link;
    int jointId;
    Selection jointType;
    Vector3 jointAxis;
    double ulimit;
    double llimit;
    bool isselected;

    SceneLinkPtr sceneLink;
    SgScaleTransformPtr defaultAxesScale;
    SgMaterialPtr axisMaterials[3];
    double axisCylinderNormalizedRadius;

    //ModelEditDraggerPtr positionDragger;
    PositionDraggerPtr positionDragger;

    JointItemImpl(JointItem* self);
    JointItemImpl(JointItem* self, Link* link);
    JointItemImpl(JointItem* self, const JointItemImpl& org);
    ~JointItemImpl();
    
    void init();
    void onSelectionChanged();
    void attachPositionDragger();
    void onDraggerStarted();
    void onDraggerDragged();
    void onUpdated();
    void onPositionChanged();
    double radius() const;
    void setRadius(double val);
    VRMLNodePtr toVRML();
    void doAssign(Item* srcItem);
    void doPutProperties(PutPropertyFunction& putProperty);
    bool setJointAxis(const std::string& value);
    bool store(Archive& archive);
    bool restore(const Archive& archive);
};

}
    

void JointItem::initializeClass(ExtensionManager* ext)
{
    static bool initialized = false;

    if(!initialized){
        ext->itemManager().registerClass<JointItem>(N_("JointItem"));
        ext->itemManager().addCreationPanel<JointItem>();
        initialized = true;
    }
}


JointItem::JointItem()
{
    impl = new JointItemImpl(this);
}


JointItemImpl::JointItemImpl(JointItem* self)
    : self(self)
{
    link = new Link();
    init();
}


JointItem::JointItem(Link* link)
{
    impl = new JointItemImpl(this, link);
}


JointItemImpl::JointItemImpl(JointItem*self, Link* link)
    : self(self)
{
    this->link = link;
    init();
}


JointItem::JointItem(const JointItem& org)
    : Item(org)
{
    impl = new JointItemImpl(this, *org.impl);
}


JointItemImpl::JointItemImpl(JointItem* self, const JointItemImpl& org)
    : self(self),
      link(org.link)
{
    init();
}


void JointItemImpl::init()
{
    jointType.resize(4);
    jointType.setSymbol(Link::ROTATIONAL_JOINT, "rotate");
    jointType.setSymbol(Link::SLIDE_JOINT, "slide");
    jointType.setSymbol(Link::FREE_JOINT, "free");
    jointType.setSymbol(Link::FIXED_JOINT, "fixed");
    jointType.setSymbol(Link::CRAWLER_JOINT, "crawler");

    jointId = link->jointId();
    jointType.selectIndex(link->jointType());
    jointAxis = link->jointAxis();
    ulimit = link->q_upper();
    llimit = link->q_lower();
    self->translation = link->translation();
    self->rotation = link->rotation();
    sceneLink = new SceneLink(new Link());
    self->setName(link->name());

    axisCylinderNormalizedRadius = 0.04;
    
    defaultAxesScale = new SgScaleTransform;
    
    for(int i=0; i < 3; ++i){
        SgMaterial* material = new SgMaterial;
        Vector3f color(0.2f, 0.2f, 0.2f);
        color[i] = 1.0f;
        material->setDiffuseColor(Vector3f::Zero());
        material->setEmissiveColor(color);
        material->setAmbientIntensity(0.0f);
        material->setTransparency(0.6f);
        axisMaterials[i] = material;
    }

    MeshGenerator meshGenerator;
    SgMeshPtr mesh = meshGenerator.generateArrow(1.8, 0.08, 0.1, 2.5);
    for(int i=0; i < 3; ++i){
        SgShape* shape = new SgShape;
        shape->setMesh(mesh);
        shape->setMaterial(axisMaterials[i]);
        
        SgPosTransform* arrow = new SgPosTransform;
        arrow->addChild(shape);
        if(i == 0){
            arrow->setRotation(AngleAxis(-PI / 2.0, Vector3::UnitZ()));
        } else if(i == 2){
            arrow->setRotation(AngleAxis( PI / 2.0, Vector3::UnitX()));
        }
        SgInvariantGroup* invariant = new SgInvariantGroup;
        invariant->setName(axisNames[i]);
        invariant->addChild(arrow);
        defaultAxesScale->addChild(invariant);
    }
    sceneLink->addChild(defaultAxesScale);

    attachPositionDragger();

    setRadius(0.15);

    self->sigUpdated().connect(boost::bind(&JointItemImpl::onUpdated, this));
    ItemTreeView::mainInstance()->sigSelectionChanged().connect(boost::bind(&JointItemImpl::onSelectionChanged, this));
    isselected = false;

    onUpdated();
}


void JointItemImpl::onSelectionChanged()
{
    ItemList<Item> items = ItemTreeView::mainInstance()->selectedItems();
    bool selected = false;
    for(size_t i=0; i < items.size(); ++i){
        Item* item = items.get(i);
        if (item == self) {
            selected = true;
        }
    }
    if (isselected != selected) {
        isselected = selected;
        //positionDragger->setDraggerAlwaysShown(selected);
        if (isselected) {
            positionDragger->setDraggerAlwaysShown(true);
        } else {
            positionDragger->setDraggerAlwaysHidden(true);
        }
    }
}


double JointItemImpl::radius() const
{
    return defaultAxesScale->scale().x();
}


void JointItemImpl::setRadius(double r)
{
    defaultAxesScale->setScale(r);
    positionDragger->setRadius(r * 1.5);
    sceneLink->notifyUpdate();
}


void JointItemImpl::attachPositionDragger()
{
    positionDragger = new ModelEditDragger;
    positionDragger->sigDragStarted().connect(boost::bind(&JointItemImpl::onDraggerStarted, this));
    positionDragger->sigPositionDragged().connect(boost::bind(&JointItemImpl::onDraggerDragged, this));
    positionDragger->adjustSize(sceneLink->untransformedBoundingBox());
    sceneLink->addChild(positionDragger);
    sceneLink->notifyUpdate();
    self->notifyUpdate();
}


void JointItemImpl::onDraggerStarted()
{
}


void JointItemImpl::onDraggerDragged()
{
    self->translation = positionDragger->draggedPosition().translation();
    self->rotation = positionDragger->draggedPosition().rotation();
    self->notifyUpdate();
}


void JointItemImpl::onUpdated()
{
    sceneLink->translation() = self->translation;
    sceneLink->rotation() = self->rotation;
    sceneLink->notifyUpdate();
}


JointItem::~JointItem()
{
    delete impl;
}


JointItemImpl::~JointItemImpl()
{
}


Link* JointItem::link() const
{
    return impl->link;
}


ItemPtr JointItem::doDuplicate() const
{
    return new JointItem(*this);
}


void JointItem::doAssign(Item* srcItem)
{
    Item::doAssign(srcItem);
    impl->doAssign(srcItem);
}


void JointItemImpl::doAssign(Item* srcItem)
{
    JointItem* srcJointItem = dynamic_cast<JointItem*>(srcItem);
#if 0
    if(srcJointItem){
        Link* srcLink = srcJointItem->link();
        link->p() = srcLink->p();
        link->R() = srcLink->R();
    }
#endif
}


SgNode* JointItem::getScene()
{
    return impl->sceneLink;
}


void JointItem::doPutProperties(PutPropertyFunction& putProperty)
{
    EditableModelBase::doPutProperties(putProperty);
    impl->doPutProperties(putProperty);
}


void JointItemImpl::doPutProperties(PutPropertyFunction& putProperty)
{
    ostringstream oss;
    putProperty.decimals(4)(_("Joint ID"), jointId);
    putProperty(_("Joint type"), jointType,
                boost::bind(&Selection::selectIndex, &jointType, _1));
    string jt(jointType.selectedSymbol());
    if (jt == "rotate" || jt == "slide") {
        putProperty(_("Joint axis"), str(jointAxis),
                    boost::bind(&JointItemImpl::setJointAxis, this, _1));
        putProperty.decimals(4)(_("Upper limit"), ulimit);
        putProperty.decimals(4)(_("Lower limit"), llimit);
    }
    putProperty.decimals(4).min(0.0)(_("Axis size"), radius(),
                                     boost::bind(&JointItemImpl::setRadius, this, _1), true);
}


bool JointItemImpl::setJointAxis(const std::string& value)
{
    Vector3 p;
    if(toVector3(value, p)){
        jointAxis = p;
        return true;
    }
    return false;
}


VRMLNodePtr JointItem::toVRML()
{
    return impl->toVRML();
}

VRMLNodePtr JointItemImpl::toVRML()
{
    VRMLJointPtr node;
    node = new VRMLJoint();
    node->defName = self->name();
    node->jointId = jointId;
    node->jointType = jointType.selectedSymbol();
    node->jointAxis = jointAxis;
    node->ulimit.clear();
    node->ulimit.push_back(ulimit);
    node->llimit.clear();
    node->llimit.push_back(llimit);
    JointItem* parentjoint = dynamic_cast<JointItem*>(self->parentItem());
    if (parentjoint) {
        Affine3 parent, child, relative;
        parent.translation() = parentjoint->translation;
        parent.linear() = parentjoint->rotation;
        child.translation() = self->translation;
        child.linear() = self->rotation;
        relative = parent.inverse() * child;
        node->translation = relative.translation();
        node->rotation = relative.rotation();
    } else {
        node->translation = self->translation;
        node->rotation = self->rotation;
    }
    for(Item* child = self->childItem(); child; child = child->nextItem()){
        EditableModelBase* item = dynamic_cast<EditableModelBase*>(child);
        if (item) {
            VRMLNodePtr childnode = item->toVRML();
            node->children.push_back(childnode);
        }
    }
    return node;
}

bool JointItem::store(Archive& archive)
{
    return impl->store(archive);
}


bool JointItemImpl::store(Archive& archive)
{
    archive.setDoubleFormat("% .6f");

    write(archive, "position", self->translation);
    write(archive, "attitude", Matrix3(self->rotation));

    return true;
}


bool JointItem::restore(const Archive& archive)
{
    return impl->restore(archive);
}


bool JointItemImpl::restore(const Archive& archive)
{
    Vector3 p;
    if(read(archive, "position", p)){
        self->translation = p;
    }
    Matrix3 R;
    if(read(archive, "attitude", R)){
        self->rotation = R;
    }

    return true;
}