#pragma once

#include <TArc/Core/ClientModule.h>
#include <TArc/Core/FieldBinder.h>

#include <Reflection/Reflection.h>

#include <memory>

class HistoryWidget;

class HistoryModule : public DAVA::ClientModule
{
public:
    HistoryModule();

protected:
    void PostInit() override;

private:
    std::unique_ptr<DAVA::FieldBinder> binder;
    std::unique_ptr<HistoryWidget> widget;

    DAVA_VIRTUAL_REFLECTION(HistoryModule, DAVA::ClientModule);
};
