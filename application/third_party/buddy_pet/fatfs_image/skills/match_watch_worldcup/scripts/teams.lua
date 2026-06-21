local M = {}

local TEAM_ROWS = {
"Mexico|Mexiko|墨西哥|A|mex",
"South Africa|Südafrika|南非|A|south-africa,rsa",
"South Korea|Südkorea|韩国|A|south-korea,korea,kor",
"Czech Republic|Tschechien|捷克|A|czech-republic,czechia,cze",
"Canada|Kanada|加拿大|B|can",
"Bosnia and Herzegovina|Bosnien|波黑|B|bosnia & herzegovina,bosnia-and-herzegovina,bosnia-herzegovina,bosnia,bih",
"Qatar|Katar|卡塔尔|B|qat",
"Switzerland|Schweiz|瑞士|B|sui,che",
"Brazil|Brasilien|巴西|C|brasil,bra,巴西队",
"Morocco|Marokko|摩洛哥|C|mar",
"Haiti|Haiti|海地|C|hti",
"Scotland|Schottland|苏格兰|C|sco,sct",
"USA|USA|美国|D|united states,united-states,america",
"Paraguay|Paraguay|巴拉圭|D|par",
"Australia|Australien|澳大利亚|D|aus",
"Turkey|Türkei|土耳其|D|tur,türkiye",
"Germany|Deutschland|德国|E|ger,deu",
"Curacao|Curaçao|库拉索|E|curaçao,cuw",
"Ivory Coast|Elfenbeinküste|科特迪瓦|E|ivory-coast,cote d'ivoire,civ",
"Ecuador|Ecuador|厄瓜多尔|E|ecu",
"Netherlands|Niederlande|荷兰|F|nld,ned,holland",
"Japan|Japan|日本|F|jpn",
"Sweden|Schweden|瑞典|F|swe",
"Tunisia|Tunesien|突尼斯|F|tun",
"Belgium|Belgien|比利时|G|bel",
"Egypt|Ägypten|埃及|G|egy",
"Iran|Iran|伊朗|G|irn",
"New Zealand|Neuseeland|新西兰|G|new-zealand,nzl",
"Spain|Spanien|西班牙|H|esp",
"Cape Verde|Kap Verde|佛得角|H|cape-verde,cpv",
"Saudi Arabia|Saudi Arabien|沙特|H|saudi-arabia,ksa,sau",
"Uruguay|Uruguay|乌拉圭|H|uru,ury",
"France|Frankreich|法国|I|fra",
"Senegal|Senegal|塞内加尔|I|sen",
"Iraq|Irak|伊拉克|I|irq",
"Norway|Norwegen|挪威|I|nor",
"Argentina|Argentinien|阿根廷|J|arg,阿根廷队",
"Algeria|Algerien|阿尔及利亚|J|alg,dza",
"Austria|Österreich|奥地利|J|aut",
"Jordan|Jordanien|约旦|J|jor",
"Portugal|Portugal|葡萄牙|K|por,prt",
"DR Congo|DR Kongo|刚果（金）|K|dr-congo,congo dr,cod,刚果金",
"Uzbekistan|Usbekistan|乌兹别克|K|uzb",
"Colombia|Kolumbien|哥伦比亚|K|col",
"England|England|英格兰|L|eng",
"Croatia|Kroatien|克罗地亚|L|cro,hrv",
"Ghana|Ghana|加纳|L|gha",
"Panama|Panama|巴拿马|L|pan",
}

local TEAM_ALIASES = {}
local OPENLIGA_FILTERS = {}
local TEAM_DISPLAY_ALIASES = {}
local TEAM_GROUPS = {}

local function trim(s)
    return tostring(s or ""):match("^%s*(.-)%s*$")
end

local function alias_key(s)
    return trim(s):lower():gsub("%s+", " "):gsub("_", "-")
end

local function add_alias(value, canonical)
    local key = alias_key(value)
    if key ~= "" then
        TEAM_ALIASES[key] = canonical
    end
end

local function add_team_row(row)
    local canonical, openliga, display, group, aliases = row:match("^([^|]*)|([^|]*)|([^|]*)|([^|]*)|(.*)$")
    if not canonical then return end

    OPENLIGA_FILTERS[canonical] = openliga
    TEAM_DISPLAY_ALIASES[canonical] = display
    TEAM_GROUPS[canonical] = group
    add_alias(canonical, canonical)
    add_alias(openliga, canonical)
    add_alias(display, canonical)
    for alias in tostring(aliases):gmatch("[^,]+") do
        add_alias(alias, canonical)
    end
end

for _, row in ipairs(TEAM_ROWS) do
    add_team_row(row)
end

function M.canonical_team(team)
    local key = alias_key(team)
    if key == "" then return "" end
    return TEAM_ALIASES[key] or trim(team)
end

function M.localized_display_team(team)
    local canonical = M.canonical_team(team)
    return TEAM_DISPLAY_ALIASES[canonical] or canonical
end

function M.openliga_filter(team)
    local canonical = M.canonical_team(team)
    return OPENLIGA_FILTERS[canonical] or canonical
end

function M.team_group(team)
    local canonical = M.canonical_team(team)
    return TEAM_GROUPS[canonical] or ""
end

return M
