#include "barbatos_gui.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>


namespace barbatos_gui {
namespace {

const ImVec4 kAccent(0.38f, 0.24f, 0.96f, 1.00f);
const ImVec4 kAccentSoft(0.31f, 0.22f, 0.60f, 0.80f);
const ImVec4 kPanel(0.075f, 0.085f, 0.11f, 0.98f);
const ImVec4 kPanelAlt(0.105f, 0.115f, 0.15f, 0.98f);
const ImVec4 kMuted(0.58f, 0.62f, 0.70f, 1.00f);
const ImVec4 kDanger(0.95f, 0.25f, 0.30f, 1.00f);

struct PaletteEntry { const char* name; ImVec4 value; };

// Exact 42 entries copied from the global initializer at 0x14f4b4 and the
// relocated name table at 0x28c010. The original widget lays these out 6 wide.
const std::array<PaletteEntry, 42> kPresetPalette{{
    {u8"紫罗兰", ImVec4(.45098042f,.33725491f,.69411767f,1)},
    {u8"克莱因蓝", ImVec4(0,.18431373f,.65490198f,1)},
    {u8"爱马仕橙", ImVec4(.99607849f,.37647063f,.06274510f,1)},
    {u8"蒂芙尼蓝", ImVec4(.50588238f,.84705889f,.81960791f,1)},
    {u8"马尔斯绿", ImVec4(.00392157f,.50196081f,.50588238f,1)},
    {u8"勃艮第红", ImVec4(.50196081f,.09411766f,.12549020f,1)},
    {u8"普鲁士蓝", ImVec4(0,.19215688f,.32549021f,1)},
    {u8"提香红", ImVec4(.69019610f,.34901962f,.13725491f,1)},
    {u8"凡戴克棕", ImVec4(.24313727f,.17647059f,.10588236f,1)},
    {u8"申布伦黄", ImVec4(.98431379f,.82352948f,.41568631f,1)},
    {u8"松绿", ImVec4(.29803923f,.45882356f,.26274511f,1)},
    {u8"玉色", ImVec4(.98039222f,.97254908f,.95686281f,1)},
    {u8"海棠红", ImVec4(.89019614f,.43529415f,.52156866f,1)},
    {u8"若竹", ImVec4(.51764709f,.73333335f,.62352943f,1)},
    {u8"绀宇", ImVec4(0,.23529413f,.45882356f,1)},
    {u8"烟墨色", ImVec4(.36470589f,.30980393f,.34117648f,1)},
    {u8"青雀头戴", ImVec4(.09803923f,.28627452f,.33333334f,1)},
    {u8"金红", ImVec4(.95686281f,.57254905f,.15294118f,1)},
    {u8"竹色", ImVec4(.12549020f,.73333335f,.63137257f,1)},
    {u8"浅松绿", ImVec4(.62745100f,.80392164f,.80000007f,1)},
    {u8"东方既白", ImVec4(.55686277f,.64313728f,.79215693f,1)},
    {u8"浅云", ImVec4(.92941183f,.92941183f,.95686281f,1)},
    {u8"栀子", ImVec4(.99607849f,.75686282f,.24705884f,1)},
    {u8"桔梗", ImVec4(.41176474f,.40000004f,.71372551f,1)},
    {u8"桃夭", ImVec4(.97254908f,.80784321f,.84313732f,1)},
    {u8"鹤顶红", ImVec4(.83137262f,.27450982f,.21176472f,1)},
    {u8"西子", ImVec4(.63529414f,.81176478f,.83529419f,1)},
    {u8"乌黑", ImVec4(.22745100f,.18039216f,.25098041f,1)},
    {u8"茶白", ImVec4(.77647066f,.72941178f,.66274512f,1)},
    {u8"番茄红", ImVec4(.62745100f,.20000002f,.23137257f,1)},
    {u8"藕白", ImVec4(.94901967f,.88235301f,.85490203f,1)},
    {u8"烟雾蓝", ImVec4(.24313727f,.31764707f,.43137258f,1)},
    {u8"奶酪黄", ImVec4(.90588242f,.77254909f,.50980395f,1)},
    {u8"棕红", ImVec4(.59607846f,.30196080f,.14901961f,1)},
    {u8"孔雀蓝", ImVec4(0,.32941177f,.41176474f,1)},
    {u8"靛青", ImVec4(0,.22352943f,.34509805f,1)},
    {u8"卡其", ImVec4(.86274517f,.79607850f,.72941178f,1)},
    {u8"梅子青", ImVec4(.44313729f,.52941179f,.44313729f,1)},
    {u8"透黄", ImVec4(.98039222f,.87058830f,.20000002f,1)},
    {u8"珊瑚", ImVec4(1,.65098041f,.62745100f,1)},
    {u8"纯白", ImVec4(1,1,1,1)},
    {u8"纯黑", ImVec4(0,0,0,1)},
}};

const char* Tr(Language language, const char* zh, const char* en) {
    return language == Language::English ? en : zh;
}

ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// The host uses Dear ImGui without misc/cpp/imgui_stdlib.cpp.  Keep the
// reconstructed GUI self-contained while retaining editable string fields.
bool InputTextString(const char* label, std::string* value) {
    std::array<char, 512> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", value->c_str());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
    *value = buffer.data();
    return true;
}

bool InputTextMultilineString(const char* label, std::string* value, const ImVec2& size) {
    std::array<char, 1024> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", value->c_str());
    if (!ImGui::InputTextMultiline(label, buffer.data(), buffer.size(), size)) return false;
    *value = buffer.data();
    return true;
}

void DrawAsset(ImDrawList* dl, ImTextureID texture, const ImVec2& min, const ImVec2& max,
               ImU32 tint = IM_COL32_WHITE) {
    if (texture != ImTextureID{}) dl->AddImage(texture, min, max, ImVec2(0,0), ImVec2(1,1), tint);
}

bool NavButton(const char* id, const char* fallback, ImTextureID texture, bool active) {
    ImGui::PushID(id);
    const ImVec2 size(72, 62);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##nav", size);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec4 fill = active ? kAccentSoft : hovered ? ImVec4(.18f,.19f,.25f,1) : ImVec4(.10f,.11f,.14f,0);
    dl->AddRectFilled(min, max, U32(fill), 14);
    if (active) dl->AddRectFilled(ImVec2(min.x,min.y+10), ImVec2(min.x+4,max.y-10), U32(kAccent), 4);
    if (texture != ImTextureID{}) {
        DrawAsset(dl, texture, ImVec2(min.x+18,min.y+12), ImVec2(min.x+50,min.y+44));
    } else {
        const ImVec2 ts = ImGui::CalcTextSize(fallback);
        dl->AddText(ImVec2(min.x+(size.x-ts.x)*.5f,min.y+21), IM_COL32(225,228,238,255), fallback);
    }
    ImGui::PopID();
    return pressed;
}

bool Toggle(const char* label, bool* value) {
    const float h = 36, switch_w = 42;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width,h));
    if (pressed) *value = !*value;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, ImVec2(min.x+width,min.y+h),
                      ImGui::IsItemHovered() ? IM_COL32(41,44,56,210) : IM_COL32(31,34,43,170), 8);
    dl->AddText(ImVec2(min.x+12,min.y+9), IM_COL32(225,228,238,255), label);
    const ImVec2 sw_min(min.x+width-switch_w-10,min.y+8), sw_max(sw_min.x+switch_w,sw_min.y+20);
    dl->AddRectFilled(sw_min,sw_max,*value?U32(kAccent):IM_COL32(74,78,91,255),10);
    dl->AddCircleFilled(ImVec2(*value?sw_max.x-10:sw_min.x+10,sw_min.y+10),7,IM_COL32_WHITE);
    return pressed;
}

bool ActionButton(const char* label, const ImVec2& size = ImVec2(0,38)) {
    ImGui::PushStyleColor(ImGuiCol_Button,kAccentSoft);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.30f,.18f,.78f,1));
    const bool pressed=ImGui::Button(label,size);
    ImGui::PopStyleColor(3);
    return pressed;
}

void SectionTitle(const char* title, const char* subtitle = nullptr) {
    ImGui::Spacing(); ImGui::TextColored(kAccent,"%s",title);
    if (subtitle) { ImGui::SameLine(); ImGui::TextColored(kMuted,"%s",subtitle); }
    ImGui::Separator();
}

void SliderRow(const char* label,float* value,float min,float max,const char* fmt="%.1f") {
    ImGui::PushID(label); ImGui::TextUnformatted(label); ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##value",value,min,max,fmt); ImGui::PopID();
}

void ColorRow(const char* label,ImVec4* color) {
    ImGui::PushID(label); ImGui::TextUnformatted(label); ImGui::SameLine();
    ImGui::ColorEdit4("##color",&color->x,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopID();
}

bool SegmentedChoice(const char* id,int* selected,const char* const* labels,const int* values,int count) {
    bool changed=false; ImGui::PushID(id);
    const float spacing=ImGui::GetStyle().ItemSpacing.x;
    const float w=std::max(70.0f,(ImGui::GetContentRegionAvail().x-spacing*(count-1))/count);
    for(int i=0;i<count;++i){
        const bool active=*selected==values[i];
        if(active) ImGui::PushStyleColor(ImGuiCol_Button,kAccent);
        if(ImGui::Button(labels[i],ImVec2(w,36))){*selected=values[i];changed=true;}
        if(active) ImGui::PopStyleColor();
        if(i+1<count) ImGui::SameLine();
    }
    ImGui::PopID(); return changed;
}

void PresetPalette(const char* id,ImVec4* color) {
    ImGui::PushID(id);
    if(ImGui::BeginTable("##palette",6,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoSavedSettings)){
        for(std::size_t i=0;i<kPresetPalette.size();++i){
            ImGui::TableNextColumn(); ImGui::PushID(static_cast<int>(i));
            if(ImGui::ColorButton("##preset",kPresetPalette[i].value,
                                  ImGuiColorEditFlags_NoTooltip|ImGuiColorEditFlags_NoDragDrop,
                                  ImVec2(-1,24))) *color=kPresetPalette[i].value;
            const bool same=U32(*color)==U32(kPresetPalette[i].value);
            if(same) ImGui::TextColored(kAccent,"%s",kPresetPalette[i].name);
            else ImGui::TextUnformatted(kPresetPalette[i].name);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void RenderHome(const TextureRegistry& textures,Language language) {
    if(textures.Get(AssetId::Character)!=ImTextureID{}){
        const float h=std::min(260.0f,ImGui::GetContentRegionAvail().y*.44f),w=h*(1960.0f/2156.0f);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x-w)*.5f);
        ImGui::Image(textures.Get(AssetId::Character),ImVec2(w,h));
    }
    SectionTitle(Tr(language,u8"主页","Home"),"Gui_RenderHomePage @ 0x1e5fe8");
    static const char* notices[]={u8"本系列产品无任何收费以及内部等",u8"切勿被骗，开发者也不会主动联系你",
        u8"本系列作品仅供学习研究",u8"使用者的任何行为与应用作者无关",
        u8"请在24小时内删除本应用 [下载代表同意本协议]",u8"若有侵权违法行为,请联系我删除内容"};
    for(const char* n:notices) ImGui::BulletText("%s",n);
}

void RenderAim(AimState& s,Language language) {
    SectionTitle(Tr(language,u8"触摸瞄准","Aiming"),Tr(language,u8"触摸位置","Aim Pos"));
    Toggle(Tr(language,u8"当前为单透模式","Only Draw"),&s.only_draw);

    if(s.only_draw){
        ImGui::TextColored(kMuted,"%s",Tr(language,u8"单透模式","Now To Only Draw"));
        return;
    }

    SectionTitle(Tr(language,u8"主要的瞄准","Main Aim"));
    Toggle(Tr(language,u8"触摸瞄准","Aiming"),&s.aiming);
    Toggle(Tr(language,u8"触摸位置","Aim Pos"),&s.aim_position);

    const char* points_zh[]={u8"头颅",u8"胸腔",u8"盆骨"};
    const char* points_en[]={"Head","Chest","Pelvis"};
    int point=static_cast<int>(s.aim_point);
    ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo(Tr(language,u8"我的瞄准部位","My Aim Player Place"),&point,
                    language==Language::English?points_en:points_zh,3)) s.aim_point=static_cast<AimPoint>(point);

    const char* panels_zh[]={u8"瞄准参数",u8"停火闪镜",u8"连点瞄准",u8"狙击瞄准"};
    const char* panels_en[]={"Main Aim","Not Shoot Auto Aiming","Continuous Click Aiming","Sniper rifle"};
    const int panel_values[]={2,11,22,33};
    int panel=static_cast<int>(s.panel);
    SectionTitle(Tr(language,u8"特殊的瞄准功能","Other Aim Func"));
    SegmentedChoice("aim_panel",&panel,language==Language::English?panels_en:panels_zh,panel_values,4);
    s.panel=static_cast<AimFunctionPanel>(panel);

    if(s.panel==AimFunctionPanel::Main){
        Toggle(Tr(language,u8"单透模式","Only Draw"),&s.auxiliary_only_draw);
        Toggle(Tr(language,u8"圈内瞄准","Circle Aim"),&s.circle_aim);
        Toggle(Tr(language,u8"辅助横拉与压枪","Finger Interference Aim"),&s.finger_interference_aim);
        Toggle(Tr(language,u8"掉血瞄准","Hit aim"),&s.hit_aim);
        Toggle(Tr(language,u8"倒地不瞄","Not aim lie down"),&s.not_aim_lying_down);
        Toggle(Tr(language,u8"人机不瞄","Not Ai"),&s.not_aim_ai);
    } else if(s.panel==AimFunctionPanel::ContinuousClick){
        Toggle(Tr(language,u8"连点瞄准","Continuous Click Aiming"),&s.continuous_click_aiming);
        Toggle(Tr(language,u8"位置[放开火键]","Pos[Move to Game Shoot Button]"),&s.move_game_shoot_button);
        Toggle(Tr(language,u8"瞄准范围内存在目标才触发[未完成]","Aim Range Have Player To Run[Not Done]"),&s.run_when_target_in_range);
        SliderRow(Tr(language,u8"秒一次","Second Once"),&s.seconds_per_click,.01f,5,"%.2f");
        SliderRow(Tr(language,u8"连点频率 [秒]","Speed [Second]"),&s.speed_seconds,.01f,5,"%.2f");
        ImGui::InputInt(Tr(language,u8"写入当前枪支编号","Write To Weapon ID"),&s.click_weapon_id);
        if(ActionButton(Tr(language,u8"写入当前枪支编号","Write To Weapon ID"),ImVec2(-1,38))) s.request_write_click_weapon_id=true;
    } else if(s.panel==AimFunctionPanel::StopScopeFlash){
        Toggle(Tr(language,u8"停火闪镜","Not Shoot Auto Aiming"),&s.not_shoot_auto_aiming);
        Toggle(Tr(language,u8"位置[放开镜键]","Pos[Move to Game Aiming Button]"),&s.move_game_aim_button);
        Toggle(Tr(language,u8"射出子弹大于五颗才触发","Shoot Bullet Count > 5 Run"),&s.run_after_five_bullets);
        const char* speed_zh[]={u8"快速[肉眼几率看不见]",u8"均速[肉眼几率看见]",u8"慢速[肉眼可以看见]"};
        const char* speed_en[]={"Fast","Average speed","Slow"};
        int speed=static_cast<int>(s.speed_type);
        if(ImGui::Combo(Tr(language,u8"闪镜类型","Type"),&speed,language==Language::English?speed_en:speed_zh,3)) s.speed_type=static_cast<AimSpeed>(speed);
    } else {
        Toggle(Tr(language,u8"狙击自瞄","Sniper rifle"),&s.sniper_rifle);
        Toggle(Tr(language,u8"触发位置(松手放开火，按下放开镜)","Pos(Move the shoot button)"),&s.move_shoot_button);
        Toggle(Tr(language,u8"瞄准之后自动开火(仅松手) [bug]","nullptr"),&s.auto_fire_after_aim_bug);
        ImGui::InputInt(Tr(language,u8"写入当前枪支编号","Write To Weapon ID"),&s.sniper_weapon_id);
        if(ActionButton(Tr(language,u8"写入当前枪支编号","Write To Weapon ID"),ImVec2(-1,38))) s.request_write_sniper_weapon_id=true;
    }

    SectionTitle(Tr(language,u8"瞄准参数","Aim Numerical Value"));
    char recoil[128]; std::snprintf(recoil,sizeof(recoil),Tr(language,u8"枪支编号 %d 的压强强度","%d Vertical Recoil Reduction"),static_cast<int>(s.vertical_recoil_reduction));
    ImGui::TextColored(kMuted,"%s",recoil);
    SliderRow(Tr(language,u8"跟枪速度","Follow Gun Speed"),&s.follow_gun_speed,0,100);
    SliderRow(Tr(language,u8"瞄准稳定性","Aim Steadiness"),&s.aim_steadiness,0,100);
    SliderRow(Tr(language,u8"总压枪强度","All Vertical Recoil Reduction"),&s.all_vertical_recoil_reduction,0,100);
    SliderRow(Tr(language,u8"水平范围","Horizontal Range"),&s.horizontal_range,0,500);
    SliderRow(Tr(language,u8"垂直范围","Vertical Range"),&s.vertical_range,0,500);
    SliderRow(Tr(language,u8"圈内额外参数","Extra parameters"),&s.extra_parameters,0,100);
    SliderRow(Tr(language,u8"腰射最大距离","Hip-Fire Aim Max Distance"),&s.hip_fire_max_distance,0,1000,"%.0f");
    SliderRow(Tr(language,u8"瞄准最大距离","All Aim Max Distance"),&s.all_aim_max_distance,0,1000,"%.0f");
    SliderRow(Tr(language,u8"限制距离的参数","Parameters that limit the distance"),&s.limiting_distance,0,1000,"%.0f");
}

void RenderDraw(DrawState& s,Language language) {
    SectionTitle(Tr(language,u8"绘制","Draw"),"Gui_RenderDrawPage @ 0x1e4090");
    if(ImGui::BeginTabBar("##draw_groups")){
        if(ImGui::BeginTabItem(Tr(language,u8"人物类绘制","Player Class Draw"))){
            Toggle(Tr(language,u8"方框","Rect Box"),&s.rect_box); Toggle(Tr(language,u8"射线","Line"),&s.line);
            Toggle(Tr(language,u8"骨骼","Skeleton"),&s.skeleton); Toggle(Tr(language,u8"名字与距离","Name And Distance"),&s.name_and_distance);
            Toggle(Tr(language,u8"血量","Health"),&s.health); Toggle(Tr(language,u8"手持武器","Player Weapon"),&s.player_weapon);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr(language,u8"非人物类绘制","Not Player Class Draw"))){
            Toggle(u8"盒子",&s.box_class); Toggle(u8"载具",&s.vehicle_class); Toggle(u8"空投",&s.airdrop_class); Toggle(u8"地铁宝箱",&s.metro_crate_class);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr(language,u8"其他类绘制","Else Class Draw"))){
            Toggle(Tr(language,u8"雷达","Radar"),&s.radar); Toggle(Tr(language,u8"被瞄提示","Player Aim You Tips"),&s.aimed_warning);
            Toggle(Tr(language,u8"背敌预警","Back Enemy Warning"),&s.back_enemy_warning); Toggle(Tr(language,u8"跳过人机","Not Draw Ai"),&s.not_draw_ai);
            Toggle(Tr(language,u8"关闭击倒跑马灯","Off Color Dynamic"),&s.disable_color_dynamic);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if(ImGui::CollapsingHeader(Tr(language,u8"调节人物绘制","Player draw Adjust"),ImGuiTreeNodeFlags_DefaultOpen)){
        SliderRow(Tr(language,u8"粗细 | 大小","Size"),&s.player_size,4,80,"%.0f");
        SliderRow(Tr(language,u8"骨骼最大显示距离","Skeleton Draw Distance Max"),&s.skeleton_max_distance,0,1000,"%.0f");
        SliderRow(Tr(language,u8"调节","Adjust"),&s.player_adjust,-100,100); ColorRow(Tr(language,u8"绘制颜色","Draw Color"),&s.player_color);
    }
    if(ImGui::CollapsingHeader(Tr(language,u8"载具","Vehicle"))){
        Toggle(Tr(language,u8"显示载具血量","Draw Vehicle State"),&s.draw_vehicle_state);
        Toggle(Tr(language,u8"显示载具血量 [bug]","Draw Vehicle State [bug]"),&s.draw_vehicle_state_bug);
        SliderRow("Size##vehicle",&s.vehicle_size,4,80,"%.0f"); SliderRow("Adjust##vehicle",&s.vehicle_adjust,-100,100);
        ColorRow("Draw Color##vehicle",&s.vehicle_color);
    }
    if(ImGui::CollapsingHeader(Tr(language,u8"雷达位置","Radar Pos"))){
        SliderRow(Tr(language,u8"扩散比例","Angle"),&s.radar_angle,0,360,"%.0f"); SliderRow("Adjust##radar",&s.radar_adjust,-100,100);
        Toggle(Tr(language,u8"雷达位置","Radar Pos"),&s.radar_position_mode); Toggle(Tr(language,u8"位置 | 大小","Pos | Size"),&s.radar_position_and_size);
    }
    if(ImGui::CollapsingHeader(Tr(language,u8"被瞄准提示大小","Player Aim You Tips Size"))){
        SliderRow("Size##aimed_tip",&s.aimed_at_tip_size,4,80,"%.0f"); SliderRow("Adjust##aimed_tip",&s.aimed_at_tip_adjust,-100,100);
        ColorRow("Draw Color##aimed_tip",&s.aimed_at_tip_color);
    }
    if(ImGui::CollapsingHeader(Tr(language,u8"健康色彩","Health Color"))){
        SliderRow(Tr(language,u8"血量透明度","Health Transparency"),&s.health_transparency,0,1,"%.2f");
        const char* hz[]={u8"圆形血量",u8"矩形血量一",u8"矩形血量二",u8"格式血量",u8"宽型血量"};
        const char* he[]={"Circular Health","Rect Health One","Rect Health Two",u8"格式血量","Wide Blood Volume"};
        int type=static_cast<int>(s.health_type); if(ImGui::Combo(Tr(language,u8"健康类型","Health Type"),&type,language==Language::English?he:hz,5)) s.health_type=static_cast<HealthType>(type);
        ColorRow(Tr(language,u8"健康色彩","Health Color"),&s.health_color);
        Toggle(Tr(language,u8"同队同色","Same team Same color"),&s.same_team_same_color);
        Toggle(Tr(language,u8"跟随血量","Follow blood volume"),&s.follow_health_color);
    }
    Toggle(Tr(language,u8"更多的色彩","More colors"),&s.show_more_colors);
    if(s.show_more_colors) PresetPalette("draw_palette",&s.player_color);
}

void RenderItems(ItemsState& s,Language language) {
    SectionTitle(Tr(language,u8"物资","Items"),"Gui_RenderItemsPage @ 0x1e69f8");
    Toggle(Tr(language,u8"显示类名","Open Class Name"),&s.open_class_name);
    Toggle(Tr(language,u8"开发者模式","Dev Mode"),&s.developer_mode);
    Toggle(Tr(language,u8"我定义的物资","My Items-Mate"),&s.my_items_mate);
    if(s.developer_mode){
        ImGui::BeginChild("##centreTwo",ImVec2(0,150),true);
        ImGui::TextWrapped(u8"自定义物资教程"); ImGui::BulletText(u8"打开开发者模式");
        ImGui::BulletText(u8"长按屏幕内你需要绘制的类名"); ImGui::BulletText(u8"如多个类名重叠可长按之后滑动选择");
        ImGui::BulletText(u8"更改名字在启动本辅助的文件内更改"); ImGui::EndChild();
    }
    SliderRow(Tr(language,u8"全部物资大小","All Items Size"),&s.all_items_size,4,80,"%.0f");
    SliderRow(Tr(language,u8"全部物资距离[100为不限制]","All Items Distance[100 to Not bound]"),&s.all_items_distance,0,1000,"%.0f");
    ColorRow(Tr(language,u8"绘制颜色","Draw Color"),&s.all_items_color);
    Toggle(Tr(language,u8"更多的色彩","More colors"),&s.show_more_colors);
    if(s.show_more_colors) PresetPalette("items_palette",&s.all_items_color);

    if(!s.custom_items.empty()) SectionTitle(Tr(language,u8"我定义的物资","Custom classes"));
    for(std::size_t i=0;i<s.custom_items.size();++i){
        ItemEntry& item=s.custom_items[i]; ImGui::PushID(static_cast<int>(i));
        Toggle(item.display_name.empty()?item.class_name.c_str():item.display_name.c_str(),&item.enabled);
        ImGui::TextColored(kMuted,"%s",item.class_name.c_str()); ImGui::SameLine();
        ImGui::ColorButton("##item_color",item.color); ImGui::SameLine();
        if(ActionButton(Tr(language,u8"调节","Adjust"),ImVec2(110,30))) item.editor_open=!item.editor_open;
        if(item.editor_open){
            InputTextString(Tr(language,u8"名称","Display name"), &item.display_name);
            SliderRow(Tr(language,u8"大小","Size"),&item.size,4,80,"%.0f");
            SliderRow(Tr(language,u8"距离","Distance"),&item.max_distance,0,1000,"%.0f");
            ColorRow(Tr(language,u8"绘制颜色","Draw Color"),&item.color); PresetPalette("item_palette",&item.color);
        }
        ImGui::Separator(); ImGui::PopID();
    }
}

void DiagnosticText(const SettingsState& s) {
    ImGui::Text("pid: %d  libUE4: 0x%" PRIxPTR,s.pid,s.libue4);
    ImGui::Text("Matrix: 0x%" PRIxPTR "  Self: 0x%" PRIxPTR,s.matrix,s.self);
    ImGui::Text("Arrayaddr: 0x%" PRIxPTR "  Count: %d",s.array_address,s.array_count);
    ImGui::Text("SelfAddr: 0x%" PRIxPTR,s.self_address);
}

void RenderFilePicker(const char* id,FilePickerState& picker,const char* extension_a,const char* extension_b,
                      std::string* selected,Language language) {
    if(!picker.open) return;
    ImGui::PushID(id); ImGui::BeginChild("##file_picker",ImVec2(0,260),true);
    ImGui::TextWrapped("%s",picker.current_directory.c_str());
    if(ActionButton(Tr(language,u8"返回上一级","Back to parent"),ImVec2(-1,34))) picker.selected_index=-1;
    int n=0;
    for(const std::string& dir:picker.directories){ ImGui::PushID(n++); if(ImGui::Selectable(dir.c_str(),false)) picker.current_directory=dir; ImGui::PopID(); }
    for(const std::string& file:picker.files){
        const bool accepted=(extension_a&&file.find(extension_a)!=std::string::npos)||(extension_b&&file.find(extension_b)!=std::string::npos);
        if(!accepted) continue;
        ImGui::PushID(n); const bool chosen=picker.selected_index==n;
        if(ImGui::Selectable(file.c_str(),chosen)){picker.selected_index=n;*selected=file;} ImGui::PopID(); ++n;
    }
    ImGui::EndChild(); ImGui::PopID();
}

void RenderSettings(SettingsState& s,const TextureRegistry& textures) {
    const Language language=s.language;
    SectionTitle(Tr(language,u8"设 置","Settings"),"Gui_RenderSettingsPage @ 0x1e8090");
    Toggle(Tr(language,u8"脱离后台运行 [KSU类自带无后台]","Leave Procs Run [KSU Self-contained]"),&s.leave_process_running);
    Toggle(Tr(language,u8"防止屏幕录制","Not Screen Recording"),&s.not_screen_recording);
    int lang=static_cast<int>(s.language);
    ImGui::RadioButton(u8"简体中文",&lang,0); ImGui::SameLine(); ImGui::RadioButton("English",&lang,1);
    s.language=static_cast<Language>(lang);

    if(ImGui::BeginTabBar("##settings_panels")){
        if(ImGui::BeginTabItem(Tr(s.language,u8"主题","Main"))){
            SectionTitle(Tr(s.language,u8"主题色","Theme Color")); ColorRow(Tr(s.language,u8"主题色","Theme Color"),&s.theme_color);
            PresetPalette("theme_palette",&s.theme_color);
            if(ActionButton(Tr(s.language,u8"程序结束入口","Kernel Exit"),ImVec2(-1,38))) s.request_kernel_exit=true;
            if(ActionButton(Tr(s.language,u8"退出 & 保存配置","Exit & Save Configuration"),ImVec2(-1,38))) s.request_exit_and_save=true;
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr(s.language,u8"灵动岛","Island"))){
            SliderRow(Tr(s.language,u8"灵动岛大小比例","Island Size"),&s.island_size,.25f,3,"%.1f");
            const char* dz[]={u8"MacOS \"神奇效果\"",u8"IpadOS \"聚散效果\""}; const char* de[]={"MacOS \"Magic effect\"","IpadOS \"Dispersion effect\""};
            int d=static_cast<int>(s.dynamic_style); if(ImGui::Combo(Tr(s.language,u8"动画","Dynamic"),&d,s.language==Language::English?de:dz,2)) s.dynamic_style=static_cast<DynamicStyle>(d);
            Toggle(Tr(s.language,u8"动画效果","Dynamic"),&s.dynamic_island); Toggle(Tr(s.language,u8"灵动岛上岛测试","Island Dynamic Test"),&s.island_dynamic_test);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr(s.language,u8"图片","Photo"))){
            if(ActionButton(Tr(s.language,u8"更换图片 [加载时间为2-30秒左右]","Change Photo"),ImVec2(-1,38))) s.change_photo=true;
            InputTextString(Tr(s.language,u8"图片路径",".png / .jpg"), &s.selected_picture_path);
            ImGui::TextWrapped("%s", Tr(s.language,u8"图片路径为启动目录\n点击对应图片即可设置","Picture path is the launch directory; click an image to set it"));
            if(ActionButton(Tr(s.language,u8"还原初始图片","Restore the original picture"),ImVec2(-1,36))) s.restore_original_picture=true;
            if(ActionButton(Tr(s.language,u8"通过文件夹选择","Use File Dir Switch"),ImVec2(-1,36))){s.use_picture_directory=true;s.picture_picker.open=true;}
            RenderFilePicker("picture",s.picture_picker,".png",".jpg",&s.selected_picture_path,s.language);
            ImGui::ProgressBar(std::clamp(s.preview_progress,0.0f,1.0f),ImVec2(-1,0),"PhotoLook");
            if(textures.Get(AssetId::Avatar)!=ImTextureID{}) ImGui::Image(textures.Get(AssetId::Avatar),ImVec2(100,100));
            if(ActionButton(Tr(s.language,u8"设置为左上角图片","Set as top-left picture"),ImVec2(-1,38))) s.request_set_picture_top_left=true;
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr(s.language,u8"字体","Text"))){
            if(ActionButton(Tr(s.language,u8"更换字体 [仅支持TTF格式]","Change Text [Use TTF Format"),ImVec2(-1,38))) s.change_text=true;
            InputTextString(".ttf / .TTF", &s.selected_font_path);
            if(ActionButton(Tr(s.language,u8"还原初始字体","Restore the original Text"),ImVec2(-1,36))) s.restore_original_text=true;
            if(ActionButton(Tr(s.language,u8"从云端获取","From the cloud"),ImVec2(-1,36))) s.load_from_cloud=true;
            if(s.load_from_cloud){ ImGui::TextUnformatted(u8"下列为云端字体"); for(const std::string& f:s.cloud_fonts) if(ImGui::Selectable(f.c_str(),s.selected_font_path==f)) s.selected_font_path=f; }
            if(ActionButton(Tr(s.language,u8"通过文件夹选择","Use File Dir Switch"),ImVec2(-1,36))){s.use_font_directory=true;s.font_picker.open=true;}
            RenderFilePicker("font",s.font_picker,".ttf",".TTF",&s.selected_font_path,s.language);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr(s.language,u8"动效","Dynamic"))){
            const char* tz[]={u8"增加一个任务[一]",u8"增加一个任务[二]",u8"增加一个任务[三]",u8"增加一个任务[四]",u8"增加一个任务[五]",u8"增加一个任务[六]"};
            for(int i=0;i<6;++i) Toggle(tz[i],&s.dynamic_tasks[static_cast<std::size_t>(i)]);
            InputTextMultilineString(Tr(s.language,u8"灵动岛文本","Island text"), &s.dynamic_long_text, ImVec2(-1,80));
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("GameData")){
            DiagnosticText(s);
            char automatic[128]; std::snprintf(automatic,sizeof(automatic),u8"自动遍历[需要性能较多] 当前索引: %d",s.automatic_index); ImGui::TextWrapped("%s",automatic);
            const char* modes[]={u8"官方基址链[基址链条索引的0]",u8"自身结构体",u8"手动选择"};
            int mode=static_cast<int>(s.game_data_mode); for(int i=0;i<3;++i) ImGui::RadioButton(modes[i],&mode,i); s.game_data_mode=static_cast<GameDataMode>(mode);
            if(s.game_data_mode!=GameDataMode::OfficialBaseChain && ActionButton(u8"打开指针表",ImVec2(-1,36))) s.request_open_pointer_table=true;
            if(s.request_open_pointer_table||!s.pointer_table.empty()){
                SectionTitle("Pointer table"); for(std::uintptr_t p:s.pointer_table) ImGui::Text("0x%" PRIxPTR,p);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void RenderUpdateDialog(UpdateDialogState& u,Language language) {
    if(!u.open) return;
    ImGui::OpenPopup("Find the new version");
    if(ImGui::BeginPopupModal("Find the new version",&u.open,ImGuiWindowFlags_None)){
        ImGui::TextColored(u.mandatory?kDanger:kMuted,"%s",u.mandatory?Tr(language,u8"[强制性更新]","[Mandatory update]"):Tr(language,u8"[非强制性更新]","[Not mandatory update]"));
        ImGui::Text("%s %s",Tr(language,u8"当前版本:","Now version:"),u.current_version.c_str());
        ImGui::Text("%s %s",Tr(language,u8"最新版本:","New version:"),u.new_version.c_str());
        if(u.fetching_size) ImGui::TextUnformatted(Tr(language,u8"正在获取大小","Fetching size"));
        else ImGui::Text("%s %.2f MB",Tr(language,u8"大小:","Size:"),u.download_size_mb);
        if(!u.message.empty()) ImGui::TextWrapped("%s",u.message.c_str());
        ImGui::ProgressBar(std::clamp(u.progress,0.0f,1.0f),ImVec2(-1,0),Tr(language,u8"下载进度:","Load progress:"));
        if(u.starting) ImGui::TextUnformatted(Tr(language,u8"正在启动","Start a new one for you"));
        if(u.completed) ImGui::TextUnformatted(Tr(language,u8"更新完成","Update Done"));
        if(!u.downloading&&!u.completed){
            if(ActionButton(Tr(language,u8"确认更新","Confirm Update"),ImVec2(160,38))){u.downloading=true;u.request_confirm=true;}
            ImGui::SameLine(); if(ActionButton(Tr(language,u8"正在更新中","Download"),ImVec2(140,38))){u.downloading=true;u.request_download=true;}
        } else if(u.completed) {
            if(ActionButton(Tr(language,u8"请求更新","Use Update"),ImVec2(160,38))) u.request_use_update=true;
        }
        if(!u.mandatory){ ImGui::SameLine(); if(ImGui::Button(Tr(language,u8"取消更新","Cancel Update"),ImVec2(140,38))){u.open=false;ImGui::CloseCurrentPopup();} }
        ImGui::EndPopup();
    }
}

void ApplyStyle(const ImVec4& accent) {
    ImGuiStyle& style=ImGui::GetStyle(); style.WindowRounding=18; style.ChildRounding=14; style.FrameRounding=9;
    style.PopupRounding=12; style.ScrollbarRounding=9; style.WindowPadding=ImVec2(12,12); style.ItemSpacing=ImVec2(8,8);
    style.Colors[ImGuiCol_WindowBg]=kPanel; style.Colors[ImGuiCol_ChildBg]=kPanelAlt;
    style.Colors[ImGuiCol_Border]=ImVec4(.25f,.26f,.34f,.60f); style.Colors[ImGuiCol_FrameBg]=ImVec4(.13f,.14f,.18f,1);
    style.Colors[ImGuiCol_FrameBgHovered]=ImVec4(.19f,.18f,.28f,1); style.Colors[ImGuiCol_FrameBgActive]=accent;
    style.Colors[ImGuiCol_CheckMark]=accent; style.Colors[ImGuiCol_SliderGrab]=accent; style.Colors[ImGuiCol_SliderGrabActive]=accent;
    style.Colors[ImGuiCol_Header]=ImVec4(accent.x,accent.y,accent.z,.65f); style.Colors[ImGuiCol_HeaderHovered]=accent;
    style.Colors[ImGuiCol_Tab]=ImVec4(.12f,.13f,.17f,1); style.Colors[ImGuiCol_TabActive]=ImVec4(accent.x,accent.y,accent.z,.70f); style.Colors[ImGuiCol_TabHovered]=accent;
}

} // namespace

void RenderBarbatosGui(GuiState& state,const TextureRegistry& textures) {
    if(!state.visible) return;
    ApplyStyle(state.settings.theme_color);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 size(std::max(360.0f, std::min(1040.0f, display.x - 24.0f)),
                      std::max(420.0f, std::min(680.0f, display.y - 24.0f)));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f), ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags flags=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoBringToFrontOnFocus;
    if(ImGui::Begin("Hello ImGui",&state.visible,flags)){
        ImGui::BeginChild("##left",ImVec2(104,0),true,ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(state.settings.theme_color,"Dome"); ImGui::TextColored(kMuted,"DitPro"); ImGui::Text("SDK: 15"); ImGui::Spacing();
        struct NavItem{Page page;AssetId asset;const char* fallback;};
        static const NavItem nav[]={{Page::Home,AssetId::Profile,"H"},{Page::Draw,AssetId::DrawEye,"D"},{Page::Aim,AssetId::AimTarget,"A"},{Page::Items,AssetId::ItemsCode,"I"},{Page::Settings,AssetId::Settings,"S"}};
        for(const NavItem& item:nav){if(NavButton(item.fallback,item.fallback,textures.Get(item.asset),state.page==item.page)) state.page=item.page;ImGui::Spacing();}
        ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("##content",ImVec2(0,0),false);
        switch(state.page){
            case Page::Home: RenderHome(textures,state.settings.language); break;
            case Page::Draw: RenderDraw(state.draw,state.settings.language); break;
            case Page::Aim: RenderAim(state.aim,state.settings.language); break;
            case Page::Items: RenderItems(state.items,state.settings.language); break;
            case Page::Settings: RenderSettings(state.settings,textures); break;
        }
        ImGui::EndChild();
    }
    ImGui::End(); RenderUpdateDialog(state.update,state.settings.language);
}

} // namespace barbatos_gui
