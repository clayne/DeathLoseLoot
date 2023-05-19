extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
#ifndef DEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= Version::PROJECT;
	*path += ".log"sv;
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef DEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_skse->IsEditor()) {
		logger::critical("Loaded in editor, marking as incompatible"sv);
		return false;
	}

	const auto ver = a_skse->RuntimeVersion();
	if (ver < SKSE::RUNTIME_1_5_39) {
		logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
		return false;
	}

	return true;
}

char* get_extraeditorID(RE::ExtraDataList* list) { return _generic_foo_<11846, decltype(get_extraeditorID)>::eval(list); }

void set_extraeditorID(RE::ExtraDataList* list, const char* name)
{
	return _generic_foo_<11845, decltype(set_extraeditorID)>::eval(list, name);
}

const char* hideTag = "Hide in inv";

bool is_tagged(RE::InventoryEntryData* item)
{
	if (auto lists = item->extraLists) {
		auto end = lists->end();
		for (auto it = lists->begin(); it != end; ++it) {
			auto& list = *it;
			if ((list->HasType<RE::ExtraWorn>() || list->HasType<RE::ExtraWornLeft>()) &&
				list->HasType<RE::ExtraTextDisplayData>()) {
				return true;
			}
		}
	}

	//auto lists = item->extraLists;
	//if (lists)
	//	for (auto it = lists->begin(); it != lists->end(); ++it)
	//		if (auto fenix = get_extraeditorID(*it); fenix && !strcmp(fenix, hideTag)) {
	//			return true;
	//		}
	return false;
}

class Settings : SettingsBase
{
	static constexpr auto ini_path = "Data/skse/plugins/DeathLosesLoot.ini"sv;

public:
	static inline std::string esp_name;

	static inline uint32_t prop_id;
	static inline uint32_t frac_gold_id;
	static inline uint32_t deny_kwd_id;

	static void ReadSettings()
	{
		CSimpleIniA ini;
		ini.LoadFile(ini_path.data());

		ReadString(ini, "General", "esp", esp_name);

		ReadUint32(ini, "General", "prop", prop_id);
		ReadUint32(ini, "General", "fracGold", frac_gold_id);
		ReadUint32(ini, "General", "denyKwd", deny_kwd_id);
	}
};

class DataHandler
{
	static inline RE::TESGlobal* prop;
	static inline RE::TESGlobal* frac_gold;
	static inline RE::BGSKeyword* deny_kwd;

public:
	static inline RE::TESObjectMISC* gold;

	static void init()
	{
		auto datahandler = RE::TESDataHandler::GetSingleton();

		const auto& esp = Settings::esp_name;

		gold = RE::TESForm::LookupByID<RE::TESObjectMISC>(0xf);

		prop = datahandler->LookupForm<RE::TESGlobal>(Settings::prop_id, esp);
		frac_gold = datahandler->LookupForm<RE::TESGlobal>(Settings::frac_gold_id, esp);
		deny_kwd = datahandler->LookupForm<RE::BGSKeyword>(Settings::deny_kwd_id, esp);
	}

	// ret true with prop = val
	static bool rnd() { return FenixUtils::random_range(0.0f, 1.0f) <= prop->value; }

	// ret cost * frac
	static float get_cost(uint32_t cost) { return cost * frac_gold->value; }

	static bool has_kwd(RE::Actor* a) { return a->GetBaseObject()->As<RE::TESNPC>()->HasKeyword(deny_kwd); }
};

class OpenInvHook
{
public:
	static void Hook()
	{
		_should_be_displayed = SKSE::GetTrampoline().write_call<5>(REL::ID(50239).address() + 0x13,
			should_be_displayed);  // SkyrimSE.exe+861560
	}

private:
	static bool should_be_displayed(RE::InventoryEntryData* item) { return _should_be_displayed(item) && !is_tagged(item); }

	static inline REL::Relocation<decltype(should_be_displayed)> _should_be_displayed;
};

void tag_item(RE::InventoryEntryData* item) {
	if (auto lists = item->extraLists) {
		auto end = lists->end();
		for (auto it = lists->begin(); it != end; ++it) {
			auto& list = *it;
			if (list->HasType<RE::ExtraWorn>() || list->HasType<RE::ExtraWornLeft>()) {
				list->Add(new RE::ExtraTextDisplayData(hideTag));
			}
		}
	}

	//auto extralist = new RE::ExtraDataList();
	//set_extraeditorID(extralist, hideTag);
	//item->AddExtraList(extralist);
}

bool should_tag(RE::InventoryEntryData* item)
{
	if (item->IsEnchanted())
		return false;

	return DataHandler::rnd();
}

void cleanLoot(RE::Actor* a) {
	if (DataHandler::has_kwd(a))
		return;

	class Visitor : public RE::InventoryChanges::IItemChangeVisitor
	{
		RE::BSContainer::ForEachResult Visit(RE::InventoryEntryData* item) override
		{
			if (should_tag(item)) {
				tag_item(item);
				sum += DataHandler::get_cost(item->object->GetGoldValue());
			}

			return RE::BSContainer::ForEachResult::kContinue;
		}

	public:
		float sum = 0;
	} visitor;

	auto changes = a->GetInventoryChanges();
	changes->VisitWornItems(visitor);

	auto count = static_cast<uint32_t>(visitor.sum);
	if (count > 0)
		FenixUtils::AddItem(a, DataHandler::gold, nullptr, count, nullptr);
}

class EventHandler : public RE::BSTEventSink<RE::TESDeathEvent>
{
public:
	static EventHandler* GetSingleton()
	{
		static EventHandler singleton;
		return std::addressof(singleton);
	}

	static void Register()
	{
		auto scriptEventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
		scriptEventSourceHolder->GetEventSource<RE::TESDeathEvent>()->AddEventSink(EventHandler::GetSingleton());
	}

	virtual RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
		RE::BSTEventSource<RE::TESDeathEvent>*) override
	{
		if (a_event && !a_event->dead)
			if (auto a = a_event->actorDying.get(); a && a->As<RE::Actor>())
				cleanLoot(a->As<RE::Actor>());

		return RE::BSEventNotifyControl::kContinue;
	}

private:
	EventHandler() = default;
	EventHandler(const EventHandler&) = delete;
	EventHandler(EventHandler&&) = delete;
	virtual ~EventHandler() = default;

	EventHandler& operator=(const EventHandler&) = delete;
	EventHandler& operator=(EventHandler&&) = delete;
};

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		Settings::ReadSettings();
		EventHandler::Register();
		OpenInvHook::Hook();
		DataHandler::init();

		break;
	}
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	auto g_messaging = reinterpret_cast<SKSE::MessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));
	if (!g_messaging) {
		logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
		return false;
	}

	logger::info("loaded");

	SKSE::Init(a_skse);
	SKSE::AllocTrampoline(1 << 10);

	g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

	return true;
}
