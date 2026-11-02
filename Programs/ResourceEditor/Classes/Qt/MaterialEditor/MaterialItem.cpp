#include "Classes/Qt/MaterialEditor/MaterialItem.h"
#include "Classes/Qt/MaterialEditor/MaterialModel.h"
#include "Classes/Qt/TextureBrowser/TextureCache.h"

#include <TArc/Core/Deprecated.h>
#include <TArc/DataProcessing/DataContext.h>
#include <TArc/SharedModules/ThemesModule/ThemesModule.h>
#include <TArc/Utils/Utils.h>

#include <QDebug>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSet>

namespace MaterialItemLocal
{
const DAVA::int32 MAX_MATERIAL_HEIGHT = 30;
}

MaterialItem::MaterialItem(DAVA::NMaterial* _material, bool dragEnabled, bool dropEnabled)
    : QObject(nullptr)
    , QStandardItem()
    , material(_material)
{
    DVASSERT(material);

    setEditable(false);
    setData(QVariant::fromValue<DAVA::NMaterial*>(material));
    setDragEnabled(dragEnabled);
    setDropEnabled(dropEnabled);
    setSizeHint(QSize(MaterialItemLocal::MAX_MATERIAL_HEIGHT, MaterialItemLocal::MAX_MATERIAL_HEIGHT));

    setColumnCount(3);
}

MaterialItem::~MaterialItem()
{
}

QVariant MaterialItem::data(int role) const
{
    QVariant ret;

    switch (role)
    {
    case Qt::DisplayRole:
        ret = QString(material->GetMaterialName().c_str());
        break;
    case Qt::DecorationRole:
        const_cast<MaterialItem*>(this)->requestPreview();
        ret = QStandardItem::data(role);
        break;
    case Qt::BackgroundRole:
    {
        if (GetFlag(MaterialItem::IS_MARK_FOR_DELETE))
        {
            ret = QBrush(QColor(255, 0, 0, 20));
        }
        else if (GetMaterial()->GetConfigCount() > 1)
        {
            ret = QBrush(QColor(0, 0, 255, 40));
        }
    }
    break;
    case Qt::ForegroundRole:
    {
        ret = QStandardItem::data(role);
        if (GetFlag(MaterialItem::IS_PART_OF_SELECTION))
        {
            QColor color = DAVA::Deprecated::GetDataNode<DAVA::ThemesSettings>()->GetHighligtedItemTextColor();
            ret = QVariant::fromValue(color);
        }
    }
    break;
    case Qt::FontRole:
    {
        ret = QStandardItem::data(role);
        if (GetFlag(MaterialItem::IS_PART_OF_SELECTION))
        {
            QFont font = ret.value<QFont>();
            font.setBold(true);
            ret = font;
        }
    }
    break;
    default:
        ret = QStandardItem::data(role);
        break;
    }

    return ret;
}

DAVA::NMaterial* MaterialItem::GetMaterial() const
{
    return material;
}

void MaterialItem::SetFlag(MaterialFlag flag, bool enable)
{
    DVASSERT((flag == IS_PART_OF_SELECTION) || (flag == IS_MARK_FOR_DELETE));

    if (enable == GetFlag(flag))
        return;

    if (flag == IS_PART_OF_SELECTION)
    {
        QFont curFont = font();
        curFont.setBold(enable);
        setFont(curFont);
    }

    if (enable)
    {
        curFlag |= flag;
    }
    else
    {
        curFlag &= ~flag;
    }
    emitDataChanged();
}

bool MaterialItem::GetFlag(MaterialFlag flag) const
{
    return (curFlag & flag) == flag;
}

void MaterialItem::SetLodIndex(DAVA::int32 index)
{
    if (index != lodIndex)
    {
        lodIndex = index;
        emitDataChanged();
    }
}

int MaterialItem::GetLodIndex() const
{
    return lodIndex;
}

void MaterialItem::SetSwitchIndex(DAVA::int32 index)
{
    if (index != switchIndex)
    {
        switchIndex = index;
        emitDataChanged();
    }
}

int MaterialItem::GetSwitchIndex() const
{
    return switchIndex;
}

void MaterialItem::requestPreview()
{
    if (isPreviewRequested)
        return;

    isPreviewRequested = true;

    DAVA::Texture* albedoTexture = material->GetEffectiveTexture(DAVA::NMaterialTextureName::TEXTURE_ALBEDO);
    if (albedoTexture == nullptr)
    {
        // no albedo: show the no-texture placeholder instead of an empty cell
        applyNoTextureIcon();
        return;
    }

    DAVA::TextureDescriptor* descriptor = albedoTexture->GetDescriptor();
    QVariant itemRef = QString(descriptor->pathname.GetAbsolutePathname().c_str());
    TextureCache::Instance()->getThumbnail(descriptor, this, "onThumbnailReady", itemRef);
}

void MaterialItem::applyNoTextureIcon()
{
    static QIcon noTextureIcon = []()
    {
        const int side = 24;
        QPixmap pix(side, side);
        pix.fill(Qt::transparent);

        QPainter painter(&pix);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // neutral card reads as a missing image, not as a white texture
        painter.setPen(QColor(85, 85, 85));
        painter.setBrush(QColor(58, 58, 58));
        painter.drawRoundedRect(QRectF(0.5, 0.5, side - 1.0, side - 1.0), 3.0, 3.0);

        painter.setPen(QPen(QColor(190, 80, 80), 1.6));
        painter.drawLine(QPointF(5.0, 5.0), QPointF(side - 5.0, side - 5.0));
        painter.drawLine(QPointF(side - 5.0, 5.0), QPointF(5.0, side - 5.0));

        return QIcon(pix);
    }();

    setIcon(noTextureIcon);
}

void MaterialItem::onThumbnailReady(const QList<QImage>& images, QVariant userData)
{
    if (images.empty() || images[0].isNull())
    {
        // no readable source for this descriptor: same placeholder,
        // so an empty cell never looks like a white texture
        applyNoTextureIcon();
        return;
    }

    QImage firstImage = images[0];
    QPainter painter(&firstImage);
    painter.setPen(QColor(0, 0, 0, 0x30));
    painter.drawRect(QRect(0, 0, firstImage.width() - 1, firstImage.height() - 1));
    setIcon(QIcon(QPixmap::fromImage(firstImage)));
}
