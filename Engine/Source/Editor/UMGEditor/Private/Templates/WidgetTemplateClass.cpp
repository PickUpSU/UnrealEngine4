// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "UMGEditorPrivatePCH.h"

#include "WidgetTemplateClass.h"
#include "IDocumentation.h"

#define LOCTEXT_NAMESPACE "UMGEditor"

FWidgetTemplateClass::FWidgetTemplateClass(TSubclassOf<UWidget> InWidgetClass)
	: WidgetClass(InWidgetClass)
{
	Name = WidgetClass->GetDisplayNameText();
}

FText FWidgetTemplateClass::GetCategory() const
{
	auto DefaultWidget = WidgetClass->GetDefaultObject<UWidget>();
	return DefaultWidget->GetToolboxCategory();
}

UWidget* FWidgetTemplateClass::Create(UWidgetTree* Tree)
{
	return Tree->ConstructWidget<UWidget>(WidgetClass);
}

const FSlateBrush* FWidgetTemplateClass::GetIcon() const
{
	UWidget* DefaultWidget = WidgetClass->GetDefaultObject<UWidget>();
	return DefaultWidget->GetEditorIcon();
}

TSharedRef<IToolTip> FWidgetTemplateClass::GetToolTip() const
{
	return IDocumentation::Get()->CreateToolTip(FText::FromString(WidgetClass->GetDescription()), nullptr, FString(TEXT("Shared/Types/")) + WidgetClass->GetName(), TEXT("Class"));
}

#undef LOCTEXT_NAMESPACE
