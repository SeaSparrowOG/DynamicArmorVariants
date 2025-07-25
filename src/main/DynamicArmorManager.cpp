#include "DynamicArmorManager.h"
#include "Ext/Actor.h"

auto DynamicArmorManager::GetSingleton() -> DynamicArmorManager*
{
	static DynamicArmorManager singleton{};
	return std::addressof(singleton);
}

void DynamicArmorManager::RegisterArmorVariant(std::string_view a_name, ArmorVariant&& a_variant)
{
	auto [it, inserted] = _variants.try_emplace(std::string(a_name), std::move(a_variant));

	if (inserted)
		return;

	auto& variant = it.value();

	if (!a_variant.Linked.empty()) {
		variant.Linked = a_variant.Linked;
	}

	if (!a_variant.DisplayName.empty()) {
		variant.DisplayName = a_variant.DisplayName;
	}

	if (a_variant.OverrideHead != ArmorVariant::OverrideOption::Undefined) {
		variant.OverrideHead = a_variant.OverrideHead;
	}

	for (auto& [form, replacement] : a_variant.ReplaceByForm) {
		variant.ReplaceByForm.insert_or_assign(form, replacement);
		variant.ReplaceByForm[form] = replacement;
	}

	for (auto& [slot, replacement] : a_variant.ReplaceBySlot) {
		variant.ReplaceBySlot[slot] = replacement;
	}
}

void DynamicArmorManager::SetCondition(
	std::string_view a_state,
	std::shared_ptr<RE::TESCondition> a_condition)
{
	_conditions.insert_or_assign(std::string(a_state), a_condition);
}

void DynamicArmorManager::VisitArmorAddons(
	RE::Actor* a_actor,
	RE::TESObjectARMA* a_armorAddon,
	std::function<void(RE::TESObjectARMA*)> a_visit) const
{
	std::unordered_map<std::string, const ArmorVariant::AddonList*> addonLists;
	std::string variantState;

	if (Ext::Actor::IsSkin(a_actor, a_armorAddon)) {
		a_visit(a_armorAddon);
		return;
	}

	for (auto& [name, variant] : _variants) {

		auto addonList = variant.GetAddonList(a_armorAddon);
		if (!addonList)
			continue;

		if (!variant.Linked.empty()) {
			addonLists.insert_or_assign(variant.Linked, addonList);
		}

		if (IsUsingVariant(a_actor, name)) {
			variantState = name;
			addonLists.try_emplace(name, addonList);
		}
	}

	if (!variantState.empty()) {
		auto& variantAddons = addonLists[variantState];
		for (auto& armorAddon : *variantAddons) {
			a_visit(armorAddon);
		}
	}
	else {
		a_visit(a_armorAddon);
	}
}

auto DynamicArmorManager::GetBipedObjectSlots(RE::Actor* a_actor, RE::TESObjectARMO* a_armor) const
	-> BipedObjectSlot
{
	if (!a_armor)
		return BipedObjectSlot::kNone;

	auto slot = a_armor->bipedModelData.bipedObjectSlots;

	auto race = a_actor ? a_actor->GetRace() : nullptr;

	if (!race)
		return slot.get();

	if (Ext::Actor::IsSkin(a_actor, a_armor)) {
		return slot.get();
	}

	auto overrideOption = ArmorVariant::OverrideOption::None;

	for (auto& armorAddon : a_armor->armorAddons) {
		for (auto& [name, variant] : _variants) {

			if (!variant.HasSlotOverrides())
				continue;

			if (!variant.WouldReplace(armorAddon))
				continue;

			if (IsUsingVariant(a_actor, name)) {
				if (util::to_underlying(variant.OverrideHead) >
				    util::to_underlying(overrideOption)) {

					overrideOption = variant.OverrideHead;
				}
			}
		}
	}

	auto headSlot = static_cast<BipedObjectSlot>(1 << race->data.headObject.get());
	auto hairSlot = static_cast<BipedObjectSlot>(1 << race->data.hairObject.get());

	switch (overrideOption) {
	case ArmorVariant::OverrideOption::ShowAll:
		slot.reset(headSlot, hairSlot);
		break;
	case ArmorVariant::OverrideOption::ShowHead:
		slot.reset(headSlot);
		break;
	case ArmorVariant::OverrideOption::HideHair:
		slot.set(headSlot);
		break;
	case ArmorVariant::OverrideOption::HideAll:
		slot.set(headSlot, hairSlot);
		break;
	}

	return slot.get();
}

auto DynamicArmorManager::IsUsingVariant(RE::Actor* a_actor, std::string a_state) const -> bool
{
	if (auto it = _variantOverrides.find(a_actor->GetFormID()); it != _variantOverrides.end()) {

		if (it->second.contains(a_state)) {
			return true;
		}
	}

	if (auto it = _conditions.find(a_state); it != _conditions.end()) {
		auto& condition = it->second;
		return condition ? condition->IsTrue(a_actor, a_actor) : false;
	}
	else {
		return false;
	}
}

auto DynamicArmorManager::GetVariants(RE::TESObjectARMO* a_armor) const -> std::vector<std::string>
{
	std::vector<std::string> result;

	if (!a_armor) {
		return result;
	}

	auto names = std::set<std::string>();

	auto linkedNames = std::set<std::string>();

	for (auto& armorAddon : a_armor->armorAddons) {
		for (auto& [name, variant] : _variants) {
			if (variant.WouldReplace(armorAddon)) {
				names.insert(name);

				if (!variant.Linked.empty()) {
					linkedNames.insert(variant.Linked);
				}
			}
		}
	}

	std::set_difference(
		names.begin(),
		names.end(),
		linkedNames.begin(),
		linkedNames.end(),
		std::back_insert_iterator(result));

	return result;
}

auto DynamicArmorManager::GetEquippedArmorsWithVariants(RE::Actor* a_actor)
	-> std::vector<RE::TESObjectARMO*>
{
	std::vector<RE::TESObjectARMO*> resultVector;
	resultVector.reserve(32);

	std::unordered_set<RE::TESObjectARMO*> resultSet;
	resultSet.reserve(32);

	for (std::uint32_t i = 0; i < 32; i++) {
		auto slot = static_cast<BipedObjectSlot>(1 << i);

		std::vector<std::string> variants;
		if (auto armor = a_actor->GetWornArmor(slot)) {
			variants = GetVariants(armor);
			if (!variants.empty()) {
				if (resultSet.insert(armor).second) {
					resultVector.push_back(armor);
				}
			}
		}
	}

	return resultVector;
}

auto DynamicArmorManager::GetDisplayName(const std::string& a_variant) const -> std::string
{
	if (auto it = _variants.find(a_variant); it != _variants.end()) {
		return it->second.DisplayName;
	}

	return ""s;
}

void DynamicArmorManager::ApplyVariant(RE::Actor* a_actor, const std::string& a_variant)
{
	// Find variant definition
	auto it = _variants.find(a_variant);
	if (it == _variants.end())
		return;

	auto& newVariant = it->second;

	// Get worn armor items
	std::unordered_set<RE::TESObjectARMO*> armorItems;
	armorItems.reserve(32);

	for (std::uint32_t i = 0; i < 32; i++) {
		auto slot = static_cast<BipedObjectSlot>(1 << i);

		if (auto armor = a_actor->GetWornArmor(slot)) {
			armorItems.insert(armor);
		}
	}

	// Consider only armor items affected by the new variant
	for (auto armorItemIterator = armorItems.begin(); armorItemIterator != armorItems.end();) {
		if (newVariant.WouldReplaceAny(*armorItemIterator)) {
			armorItemIterator = armorItems.erase(armorItemIterator);
		}
		else {
			++armorItemIterator;
		}
	}

	if (armorItems.empty())
		return;

	// Remove previous overrides that affect the same armor items
	auto [elem, inserted] = _variantOverrides.try_emplace(
		a_actor->GetFormID(),
		std::unordered_set<std::string>());

	auto& overrides = elem->second;

	for (auto& [name, variant] : _variants) {
		if (&variant == &newVariant)
			continue;

		if (overrides.contains(name)) {
			for (auto& armor : armorItems) {
				if (variant.WouldReplaceAny(armor)) {
					overrides.erase(name);
					break;
				}
			}
		}
	}

	// Add the new override
	overrides.insert(a_variant);
	Ext::Actor::Update3DSafe(a_actor);
}

void DynamicArmorManager::ApplyVariant(
	RE::Actor* a_actor,
	const RE::TESObjectARMO* a_armor,
	const std::string& a_variant)
{
	// Find variant definition
	auto it = _variants.find(a_variant);
	if (it == _variants.end())
		return;

	auto& newVariant = it->second;

	if (!a_actor->GetWornArmor(a_armor->GetFormID()))
		return;

	if (!newVariant.WouldReplaceAny(a_armor))
		return;

	// Remove previous overrides that affect the armor
	auto [elem, inserted] = _variantOverrides.try_emplace(
		a_actor->GetFormID(),
		std::unordered_set<std::string>());

	auto& overrides = elem->second;

	for (auto& [name, variant] : _variants) {
		if (&variant == &newVariant)
			continue;

		if (overrides.contains(name)) {
			if (variant.WouldReplaceAny(a_armor)) {
				overrides.erase(name);
			}
		}
	}

	// Add the new override
	overrides.insert(a_variant);
	Ext::Actor::Update3DSafe(a_actor);
}

void DynamicArmorManager::ResetVariant(RE::Actor* a_actor, const RE::TESObjectARMO* a_armor)
{
	auto it = _variantOverrides.find(a_actor->GetFormID());
	if (it == _variantOverrides.end())
		return;

	auto& overrides = it->second;

	for (auto& [name, variant] : _variants) {
		if (overrides.contains(name)) {
			if (variant.WouldReplaceAny(a_armor)) {
				overrides.erase(name);
			}
		}
	}

	Ext::Actor::Update3DSafe(a_actor);
}

void DynamicArmorManager::ResetAllVariants(RE::Actor* a_actor)
{
	_variantOverrides.erase(a_actor->GetFormID());
	Ext::Actor::Update3DSafe(a_actor);
}

void DynamicArmorManager::ResetAllVariants(RE::FormID a_formID)
{
	_variantOverrides.erase(a_formID);
}
