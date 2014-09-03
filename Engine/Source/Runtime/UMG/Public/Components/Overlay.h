// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Overlay.generated.h"

UCLASS(ClassGroup=UserInterface)
class UMG_API UOverlay : public UPanelWidget
{
	GENERATED_UCLASS_BODY()

	virtual void ReleaseNativeWidget() override;

#if WITH_EDITOR
	virtual const FSlateBrush* GetEditorIcon() override;
	virtual const FText GetToolboxCategory() override;
#endif

protected:

	// UPanelWidget
	virtual UClass* GetSlotClass() const override;
	virtual void OnSlotAdded(UPanelSlot* Slot) override;
	virtual void OnSlotRemoved(UPanelSlot* Slot) override;
	// End UPanelWidget

protected:

	TSharedPtr<class SOverlay> MyOverlay;

protected:
	// UWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	// End of UWidget interface
};
