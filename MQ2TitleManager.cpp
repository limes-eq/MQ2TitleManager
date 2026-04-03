#include <mq/Plugin.h>
#include <imgui/imgui.h>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

static std::mutex s_titlesMutex;

PreSetup("MQ2TitleManager");
PLUGIN_VERSION(1.0);

using namespace mq;
using namespace eqlib;
using namespace UdpLibrary;

// RoF2 2013-05-21
#define CEverQuest__HandleWorldMessage_x 0x004C3250

//----------------------------------------------------------------------------
// Opcodes (RoF2)
//----------------------------------------------------------------------------
static constexpr uint32_t OP_RequestTitles = 0x6344;
static constexpr uint32_t OP_SendTitleList = 0x2d08;
static constexpr uint32_t OP_SetTitle = 0x6527;

//----------------------------------------------------------------------------
// State
//----------------------------------------------------------------------------
struct TitleEntry {
	uint32_t    id;
	std::string prefix;
	std::string suffix;
};

static std::vector<TitleEntry> s_titles;
static bool                    s_titlesReady = false;
static bool                    s_windowOpen = false;
static UdpConnection* s_pConnection = nullptr;
static char                    s_filterBuf[128] = {};
enum class TitleSortMode { Name, ID, Length };
static TitleSortMode s_sortMode = TitleSortMode::Name;
static bool s_pendingTitlesRequest = false;

//----------------------------------------------------------------------------
// Outbound packet helpers
// Wire format: [uint16_t opcode][payload]
//----------------------------------------------------------------------------
static void SendToServer(uint32_t opcode, const void* payload, int payloadLen)
{
	if (!s_pConnection)
	{
		WriteChatf("\ar[MQ2TitleManager]\ax No connection.");
		return;
	}

	int totalLen = 2 + payloadLen;
	std::vector<uint8_t> buf(totalLen);
	uint16_t op16 = static_cast<uint16_t>(opcode);
	memcpy(buf.data(), &op16, 2);
	if (payload && payloadLen > 0)
		memcpy(buf.data() + 2, payload, payloadLen);

	s_pConnection->Send(cUdpChannelReliable1, buf.data(), totalLen);
}

static void SendRequestTitles()
{
	if (!s_pConnection)
	{
		s_pendingTitlesRequest = true;
		WriteChatf("\ay[MQ2Titles]\ax No connection yet — will request titles automatically when connected.");
		return;
	}
	SendToServer(OP_RequestTitles, nullptr, 0);
}

#pragma pack(push, 1)
struct SetTitle_Payload {
	uint32_t isSuffix;  // 0 = prefix, 1 = suffix
	uint32_t titleID;
};
#pragma pack(pop)

static void SendSetTitle(uint32_t titleID, bool isSuffix)
{
	SetTitle_Payload payload;
	payload.isSuffix = isSuffix ? 1 : 0;
	payload.titleID = titleID;

	SendToServer(OP_SetTitle, &payload, sizeof(payload));
}


//----------------------------------------------------------------------------
// ImGui Window
//----------------------------------------------------------------------------
static int   s_selectedPrefixIdx = -2;
static int   s_selectedSuffixIdx = -2;
static bool  s_selectionDirty = false;
static char  s_prefixFilterBuf[128] = {};
static char  s_suffixFilterBuf[128] = {};
static bool  s_scrollToPrefixSel = false;
static bool  s_scrollToSuffixSel = false;
static bool  s_showAppliedMsg = false;
static DWORD s_appliedMsgExpiry = 0;

static void SyncSelectionsToCurrentTitles()
{
	std::lock_guard<std::mutex> lock(s_titlesMutex);

	s_selectedPrefixIdx = -1;
	s_selectedSuffixIdx = -1;

	if (!pLocalPlayer) return;

	const std::string curPrefix = pLocalPlayer->Title;
	const std::string curSuffix = pLocalPlayer->Suffix;

	for (int i = 0; i < (int)s_titles.size(); i++)
	{
		if (!curPrefix.empty() && s_titles[i].prefix == curPrefix)
			s_selectedPrefixIdx = i;
		if (!curSuffix.empty() && s_titles[i].suffix == curSuffix)
			s_selectedSuffixIdx = i;
	}

	s_selectionDirty = false;
	s_scrollToPrefixSel = true;
	s_scrollToSuffixSel = true;
}

static void DrawTitlesWindow()
{
	if (!s_windowOpen) return;

	// Check if applied message has expired
	if (s_showAppliedMsg && GetTickCount() > s_appliedMsgExpiry)
		s_showAppliedMsg = false;

	ImGui::SetNextWindowSize(ImVec2(540, 500), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("MQ Title Manager", &s_windowOpen))
	{
		ImGui::End();
		return;
	}

	const char* charName = pLocalPlayer ? pLocalPlayer->Name : "";
	const char* curPrefix = pLocalPlayer ? pLocalPlayer->Title : "";
	const char* curSuffix = pLocalPlayer ? pLocalPlayer->Suffix : "";

	// Current title
	ImGui::Text("Current:  %s%s%s%s%s",
		curPrefix[0] ? curPrefix : "",
		curPrefix[0] ? " " : "",
		charName,
		curSuffix[0] ? " " : "",
		curSuffix[0] ? curSuffix : "");

	// Preview
	if (s_selectionDirty)
	{
		const char* previewPrefix = (s_selectedPrefixIdx >= 0 && s_selectedPrefixIdx < (int)s_titles.size())
			? s_titles[s_selectedPrefixIdx].prefix.c_str() : "";
		const char* previewSuffix = (s_selectedSuffixIdx >= 0 && s_selectedSuffixIdx < (int)s_titles.size())
			? s_titles[s_selectedSuffixIdx].suffix.c_str() : "";

		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Preview:  %s%s%s%s%s",
			previewPrefix[0] ? previewPrefix : "",
			previewPrefix[0] ? " " : "",
			charName,
			previewSuffix[0] ? " " : "",
			previewSuffix[0] ? previewSuffix : "");
	}
	else if (s_showAppliedMsg)
	{
		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Title updated!");
	}
	else
	{
		// Blank line so layout doesn't jump
		ImGui::TextUnformatted("");
	}

	ImGui::Separator();

	// Buttons row
// Apply Changes
	const bool applyDisabled = !s_selectionDirty;
	if (applyDisabled) ImGui::BeginDisabled();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.4f, 0.1f, 1.0f));
	if (ImGui::Button("Apply Changes"))
	{
		if (s_pConnection)
		{
			if (s_selectedPrefixIdx == -1)
				SendSetTitle(0, false);
			else if (s_selectedPrefixIdx >= 0 && s_selectedPrefixIdx < (int)s_titles.size())
				SendSetTitle(s_titles[s_selectedPrefixIdx].id, false);

			if (s_selectedSuffixIdx == -1)
				SendSetTitle(0, true);
			else if (s_selectedSuffixIdx >= 0 && s_selectedSuffixIdx < (int)s_titles.size())
				SendSetTitle(s_titles[s_selectedSuffixIdx].id, true);

			s_selectionDirty = false;
			s_selectedPrefixIdx = -2;
			s_selectedSuffixIdx = -2;
			s_showAppliedMsg = true;
			s_appliedMsgExpiry = GetTickCount() + 2000;
		}
	}
	ImGui::PopStyleColor(3);

	if (applyDisabled) ImGui::EndDisabled();

	ImGui::SameLine();

	// Cancel
	if (s_selectionDirty)
	{
		if (ImGui::Button("Cancel"))
			SyncSelectionsToCurrentTitles();
		ImGui::SameLine();
	}

	// Clear All
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.15f, 0.15f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.05f, 0.05f, 1.0f));
	if (ImGui::Button("Clear Current Title"))
	{
		if (s_pConnection)
		{
			SendSetTitle(0, false);
			SendSetTitle(0, true);

			s_selectedPrefixIdx = -1;
			s_selectedSuffixIdx = -1;
			s_selectionDirty = false;
			s_showAppliedMsg = true;
			s_appliedMsgExpiry = GetTickCount() + 2000;
		}
	}
	ImGui::PopStyleColor(3);

	ImGui::SameLine();

	// Refresh pushed to the right
	const float refreshWidth = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
	const float availableWidth = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availableWidth - refreshWidth);

	if (ImGui::Button("Refresh"))
	{
		s_titlesReady = false;
		s_selectionDirty = false;
		s_showAppliedMsg = false;
		s_titles.clear();
		SendRequestTitles();
	}

	if (!s_titlesReady)
	{
		if (s_pendingTitlesRequest)
			ImGui::TextDisabled("Waiting for connection...");
		else
			ImGui::TextDisabled("Waiting for server...");
	}
	else
		ImGui::Text("%d titles", (int)s_titles.size());

	ImGui::Separator();

	// Two filter boxes side by side
	ImGui::Columns(2, "filtercols", false);

	ImGui::SetNextItemWidth(-28);  // leave room for X button
	ImGui::InputText("##prefixfilter", s_prefixFilterBuf, sizeof(s_prefixFilterBuf));
	ImGui::SameLine();
	if (ImGui::Button("X##cpf"))
		memset(s_prefixFilterBuf, 0, sizeof(s_prefixFilterBuf));

	ImGui::NextColumn();

	ImGui::SetNextItemWidth(-28);  // leave room for X button
	ImGui::InputText("##suffixfilter", s_suffixFilterBuf, sizeof(s_suffixFilterBuf));
	ImGui::SameLine();
	if (ImGui::Button("X##csf"))
		memset(s_suffixFilterBuf, 0, sizeof(s_suffixFilterBuf));

	ImGui::NextColumn();
	ImGui::Columns(1);

	// Sort dropdown
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Sort by:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120);
	const char* sortItems[] = { "Name", "ID", "Length" };
	int sortCurrent = static_cast<int>(s_sortMode);
	if (ImGui::Combo("##sort", &sortCurrent, sortItems, IM_ARRAYSIZE(sortItems)))
		s_sortMode = static_cast<TitleSortMode>(sortCurrent);

	ImGui::Separator();

	// Build filtered index lists
	auto makeFilter = [](const char* buf) -> std::string {
		std::string f = buf;
		std::transform(f.begin(), f.end(), f.begin(), ::tolower);
		return f;
		};

	auto matches = [](const std::string& s, const std::string& filter) -> bool {
		if (filter.empty()) return true;
		std::string lower = s;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		return lower.find(filter) != std::string::npos;
		};

	const std::string prefixFilter = makeFilter(s_prefixFilterBuf);
	const std::string suffixFilter = makeFilter(s_suffixFilterBuf);

	std::lock_guard<std::mutex> lock(s_titlesMutex);

	std::vector<int> prefixIndices, suffixIndices;
	for (int i = 0; i < (int)s_titles.size(); i++)
	{
		if (!s_titles[i].prefix.empty() && matches(s_titles[i].prefix, prefixFilter))
			prefixIndices.push_back(i);
		if (!s_titles[i].suffix.empty() && matches(s_titles[i].suffix, suffixFilter))
			suffixIndices.push_back(i);
	}

	// Sort both lists by the same mode
	auto prefixSorter = [&](int a, int b) -> bool {
		const std::string& sa = s_titles[a].prefix;
		const std::string& sb = s_titles[b].prefix;
		switch (s_sortMode)
		{
		case TitleSortMode::ID:     return s_titles[a].id < s_titles[b].id;
		case TitleSortMode::Length: return sa.length() > sb.length();
		case TitleSortMode::Name:
		default:
		{
			std::string la = sa, lb = sb;
			std::transform(la.begin(), la.end(), la.begin(), ::tolower);
			std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
			return la < lb;
		}
		}
		};

	auto suffixSorter = [&](int a, int b) -> bool {
		const std::string& sa = s_titles[a].suffix;
		const std::string& sb = s_titles[b].suffix;
		switch (s_sortMode)
		{
		case TitleSortMode::ID:     return s_titles[a].id < s_titles[b].id;
		case TitleSortMode::Length: return sa.length() > sb.length();
		case TitleSortMode::Name:
		default:
		{
			std::string la = sa, lb = sb;
			std::transform(la.begin(), la.end(), la.begin(), ::tolower);
			std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
			return la < lb;
		}
		}
		};

	std::sort(prefixIndices.begin(), prefixIndices.end(), prefixSorter);
	std::sort(suffixIndices.begin(), suffixIndices.end(), suffixSorter);

	// Column headers
	ImGui::Columns(2, "titlecols");
	ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
		"Prefixes (%d)", (int)prefixIndices.size());
	ImGui::NextColumn();
	ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
		"Suffixes (%d)", (int)suffixIndices.size());
	ImGui::NextColumn();
	ImGui::Separator();

	const float childHeight = ImGui::GetContentRegionAvail().y;

	// Prefix child
	ImGui::BeginChild("##prefixes", ImVec2(0, childHeight), false);

	if (ImGui::Selectable("(None)##pNone", s_selectedPrefixIdx == -1))
	{
		s_selectedPrefixIdx = -1;
		s_selectionDirty = true;
	}

	for (int idx : prefixIndices)
	{
		const bool isSelected = (s_selectedPrefixIdx == idx);
		const std::string label = s_titles[idx].prefix + "##p" + std::to_string(idx);

		if (ImGui::Selectable(label.c_str(), isSelected))
		{
			s_selectedPrefixIdx = idx;
			s_selectionDirty = true;
		}

		if (isSelected)
		{
			ImGui::SetItemDefaultFocus();
			if (s_scrollToPrefixSel)
			{
				ImGui::SetScrollHereY(0.5f);
				s_scrollToPrefixSel = false;
			}
		}
	}
	ImGui::EndChild();

	ImGui::NextColumn();

	// Suffix child
	ImGui::BeginChild("##suffixes", ImVec2(0, childHeight), false);

	if (ImGui::Selectable("(None)##sNone", s_selectedSuffixIdx == -1))
	{
		s_selectedSuffixIdx = -1;
		s_selectionDirty = true;
	}

	for (int idx : suffixIndices)
	{
		const bool isSelected = (s_selectedSuffixIdx == idx);
		const std::string label = s_titles[idx].suffix + "##s" + std::to_string(idx);

		if (ImGui::Selectable(label.c_str(), isSelected))
		{
			s_selectedSuffixIdx = idx;
			s_selectionDirty = true;
		}

		if (isSelected)
		{
			ImGui::SetItemDefaultFocus();
			if (s_scrollToSuffixSel)
			{
				ImGui::SetScrollHereY(0.5f);
				s_scrollToSuffixSel = false;
			}
		}
	}
	ImGui::EndChild();

	ImGui::Columns(1);
	ImGui::End();
}

//----------------------------------------------------------------------------
// Commands
//----------------------------------------------------------------------------
static void Cmd_Titles(PlayerClient* /*pChar*/, const char* /*szLine*/)
{
	s_windowOpen = !s_windowOpen;
	if (s_windowOpen)
	{
		s_showAppliedMsg = false;
		if (!s_titlesReady)
			SendRequestTitles();  // will queue if no connection yet
		else
			SyncSelectionsToCurrentTitles();
	}
}

//----------------------------------------------------------------------------
// Packet parsing
//----------------------------------------------------------------------------
static void ParseTitleList(const char* data, uint32_t length)
{
	std::vector<TitleEntry> newTitles;

	const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
	const uint8_t* end = p + length;

	// Skip the leading count uint32
	if (p + sizeof(uint32_t) > end) return;
	uint32_t count;
	memcpy(&count, p, sizeof(uint32_t));
	p += sizeof(uint32_t);

	while (p + sizeof(uint32_t) <= end)
	{
		TitleEntry e;

		// titleID
		memcpy(&e.id, p, sizeof(uint32_t));
		p += sizeof(uint32_t);

		// prefix (null-terminated, may be empty)
		const char* s = reinterpret_cast<const char*>(p);
		size_t      max = static_cast<size_t>(end - p);
		size_t      len = strnlen(s, max);
		if (p + len >= end) break;
		e.prefix = s;
		p += len + 1;

		// suffix (null-terminated, may be empty)
		s = reinterpret_cast<const char*>(p);
		max = static_cast<size_t>(end - p);
		len = strnlen(s, max);
		if (p + len > end) break;
		e.suffix = s;
		p += len + 1;

		newTitles.push_back(std::move(e));
	}

	// Only lock for the swap
	{
		std::lock_guard<std::mutex> lock(s_titlesMutex);
		s_titles = std::move(newTitles);
		s_titlesReady = true;
	}

	SyncSelectionsToCurrentTitles();
	WriteChatf("\ay[MQ2Titles]\ax Loaded %d titles.", (int)s_titles.size());
}

//----------------------------------------------------------------------------
// Detour — CEverQuest::HandleWorldMessage
//----------------------------------------------------------------------------
DETOUR_TRAMPOLINE_DEF(unsigned char __fastcall, HandleWorldMessage_Trampoline,
	(CEverQuest*, void*, UdpConnection*, uint32_t, char*, uint32_t))

	unsigned char __fastcall HandleWorldMessage_Detour(
		CEverQuest* pThis,
		void* edx,
		UdpConnection* pConn,
		uint32_t       opcode,
		char* data,
		uint32_t       length)
{
	if (pConn && !s_pConnection)
	{
		s_pConnection = pConn;
		WriteChatf("\ay[MQ2Titles]\ax Connection captured.");

		// Fire any pending request immediately
		if (s_pendingTitlesRequest)
		{
			s_pendingTitlesRequest = false;
			SendRequestTitles();
		}
	}

	if (opcode == OP_SendTitleList && data && length > 0)
		ParseTitleList(data, length);

	return HandleWorldMessage_Trampoline(pThis, edx, pConn, opcode, data, length);
}
//----------------------------------------------------------------------------
// Plugin lifecycle
//----------------------------------------------------------------------------
PLUGIN_API void InitializePlugin()
{
	DWORD HandleWorldMessage = FixEQGameOffset(CEverQuest__HandleWorldMessage_x);

	EzDetour(HandleWorldMessage,
		HandleWorldMessage_Detour,
		HandleWorldMessage_Trampoline);

	AddCommand("/titles", Cmd_Titles, false, true, true);
	//AddCommand("/titlecapture", Cmd_TitleCapture, false, true, true);
	WriteChatf("\ay[MQ2TitleManager]\ax Loaded. Use /titles to toggle the title browser.");
}

PLUGIN_API void ShutdownPlugin()
{
	DWORD HandleWorldMessage = FixEQGameOffset(CEverQuest__HandleWorldMessage_x);
	RemoveDetour(HandleWorldMessage);
	RemoveCommand("/titles");
	//RemoveCommand("/titlecapture");
	s_pConnection = nullptr;
	s_titles.clear();
	s_titlesReady = false;
	s_windowOpen = false;
}

PLUGIN_API void OnUpdateImGui()
{
	if (GetGameState() == GAMESTATE_INGAME)
		DrawTitlesWindow();
}

PLUGIN_API void OnZoned()
{
	s_pConnection = nullptr;
	s_titlesReady = false;
	s_pendingTitlesRequest = false;
	s_titles.clear();

	// Re-queue if window is open
	if (s_windowOpen)
		SendRequestTitles();
}
