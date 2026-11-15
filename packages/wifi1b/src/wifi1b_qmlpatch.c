/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * wifi1b_qmlpatch — add Wifi1b's page/tile to the live AppHomeScreen QML.
 *
 * The installer runs this on the car.  It is additive and preserves every
 * existing tile, including WiFi, Battery and Tweaks.  It only touches the
 * file named on the command line; the installer owns backup/rollback.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 2);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    buf[got] = 0;
    *len = got;
    return buf;
}

static int write_atomic(const char *path, const char *buf, size_t len)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.wifi1btmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t at = 0;
    while (at < len) {
        ssize_t w = write(fd, buf + at, len - at);
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        at += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0) { unlink(tmp); return -1; }
    chmod(tmp, 0644);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static size_t find_block_end(const char *s, size_t n, size_t start)
{
    size_t i = start;
    while (i < n && s[i] != '{') i++;
    if (i >= n) return 0;
    int depth = 0;
    char ins = 0;
    bool esc = false, lc = false, bc = false;
    for (; i < n; i++) {
        char c = s[i], nx = i + 1 < n ? s[i + 1] : 0;
        if (lc) { if (c == '\n') lc = false; continue; }
        if (bc) { if (c == '*' && nx == '/') { bc = false; i++; } continue; }
        if (ins) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == ins) ins = 0;
            continue;
        }
        if (c == '/' && nx == '/') { lc = true; i++; continue; }
        if (c == '/' && nx == '*') { bc = true; i++; continue; }
        if (c == '"' || c == '\'') { ins = c; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return i + 1;
        }
    }
    return 0;
}

static bool insert_at(char **bufp, size_t *lenp, size_t at, const char *ins)
{
    size_t n = *lenp, il = strlen(ins);
    char *nb = realloc(*bufp, n + il + 2);
    if (!nb) return false;
    memmove(nb + at + il, nb + at, n - at + 1);
    memcpy(nb + at, ins, il);
    *bufp = nb;
    *lenp = n + il;
    return true;
}

static bool insert_after_block(char **bufp, size_t *lenp, size_t pos, const char *ins)
{
    size_t n = *lenp;
    size_t end = find_block_end(*bufp, n, pos);
    if (!end) return false;
    size_t il = strlen(ins);
    char *nb = realloc(*bufp, n + il + 2);
    if (!nb) return false;
    memmove(nb + end + il, nb + end, n - end + 1);
    memcpy(nb + end, ins, il);
    *bufp = nb;
    *lenp = n + il;
    return true;
}

static bool id_matches(const char *s, size_t n, size_t pos, const char *prefix)
{
    if (pos + 4 >= n || strncmp(s + pos, "id:", 3)) return false;
    size_t i = pos + 3;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    size_t pl = strlen(prefix);
    if (i + pl > n || strncmp(s + i, prefix, pl)) return false;
    i += pl;
    while (i < n && isdigit((unsigned char)s[i])) i++;
    return i >= n || !(isalnum((unsigned char)s[i]) || s[i] == '_');
}

static int count_custom_tiles(const char *s, size_t n)
{
    int count = 0;
    static const char *names[] = {"helloTile", "batteryTile", "wifiTile", "tweaksTile", "rebootTile", "wifi2Tile", NULL};
    for (int k = 0; names[k]; k++) {
        const char *p = s;
        while ((p = strstr(p, "id:")) != NULL) {
            if (id_matches(s, n, (size_t)(p - s), names[k])) { count++; break; }
            p++;
        }
    }
    return count;
}

/* Find the block with a given QML type and id.  Returns start/end offsets. */
static bool find_typed_id_block(const char *s, size_t n, const char *type,
                                const char *id, size_t *start, size_t *end)
{
    const char *p = s;
    while ((p = strstr(p, type)) != NULL) {
        size_t st = (size_t)(p - s);
        size_t en = find_block_end(s, n, st);
        if (en) {
            for (size_t i = st; i + 4 < en; i++) {
                if (s[i] == 'i' && !strncmp(s + i, "id:", 3) &&
                    id_matches(s, n, i, id)) {
                    *start = st; *end = en; return true;
                }
            }
        }
        p += strlen(type);
    }
    return false;
}

static bool append_page(char **bufp, size_t *lenp)
{
    if (strstr(*bufp, "id: wifi1bPage")) return true;
    const char *page =
        "\n    Wifi1bPage {\n"
        "        id: wifi1bPage\n"
        "        x: 0\n"
        "        y: ConstantModel.MENU_BAR_Y\n"
        "        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH\n"
        "        height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y\n"
        "        z: 2000\n"
        "    }\n";
    size_t n = *lenp;
    while (n > 0 && isspace((unsigned char)(*bufp)[n - 1])) n--;
    if (n == 0 || (*bufp)[n - 1] != '}') return false;
    size_t last = n - 1;
    size_t pl = strlen(page);
    char *nb = realloc(*bufp, *lenp + pl + 2);
    if (!nb) return false;
    memmove(nb + last + pl, nb + last, *lenp - last + 1);
    memcpy(nb + last, page, pl);
    *bufp = nb;
    *lenp += pl;
    return true;
}

static bool add_marker(char **bufp, size_t *lenp)
{
    if (strstr(*bufp, "WIFI1B_PAYLOAD_260128")) return true;
    const char *mark = "// WIFI1B_PAYLOAD_260128\n";
    size_t ml = strlen(mark);
    char *nb = malloc(*lenp + ml + 2);
    if (!nb) return false;
    if (!strncmp(*bufp, "//", 2)) {
        char *nl = strchr(*bufp, '\n');
        size_t at = nl ? (size_t)(nl - *bufp) + 1 : *lenp;
        memcpy(nb, *bufp, at);
        memcpy(nb + at, mark, ml);
        memcpy(nb + at + ml, *bufp + at, *lenp - at + 1);
    } else {
        memcpy(nb, mark, ml);
        memcpy(nb + ml, *bufp, *lenp + 1);
    }
    free(*bufp);
    *bufp = nb;
    *lenp += ml;
    return true;
}

static bool patch_advanced(char **bufp, size_t *lenp)
{
    const char *s = *bufp;
    int index = count_custom_tiles(s, *lenp);
    for (int page = 0; page < 3; page++) {
        int num = page + 1;
        /* Anchor after whichever sibling tile ends later on this page.
         * WiFi-based layouts end with Wifi2Tile/WifiTile; a standalone
         * install (no WiFi app) ends with Reboot/Tweaks/Battery/Hello. */
        size_t best_st = 0, best_en = 0;
        const char *sibs[] = {"Wifi2Tile", "WifiTile", "RebootTile", "TweaksTile",
                             "BatteryTile", "HelloTile", NULL};
        for (int k = 0; sibs[k]; k++) {
            const char *p = s;
            while ((p = strstr(p, sibs[k])) != NULL) {
                size_t st = (size_t)(p - s);
                size_t en = find_block_end(s, *lenp, st);
                /* The tile id is the type name with a lowercase first
                 * letter (WifiTile -> wifiTile, RebootTile -> rebootTile). */
                char want[32];
                snprintf(want, sizeof(want), "%c%s%d",
                         (char)(sibs[k][0] | 0x20), sibs[k] + 1, num);
                bool match = false;
                for (size_t i = st; en && i + 4 < en; i++)
                    if (s[i] == 'i' && !strncmp(s + i, "id:", 3) &&
                        id_matches(s, *lenp, i, want)) { match = true; break; }
                if (match && en > best_en) { best_st = st; best_en = en; }
                p += strlen(sibs[k]);
            }
        }
        if (!best_en) continue;
        char ins[1200];
        const char *menu = num == 1 ? "menuPage" : num == 2 ? "menuPage2" : "menuPage3";
        snprintf(ins, sizeof(ins),
            "\n                Wifi1bTile {\n"
            "                    id: wifi1bTile%d\n"
            "                    property var slot: root.firstEmptySlot(%d)\n"
            "                    visible: slot !== null && slot !== undefined && slot.page === %d\n"
            "                    x: %s.x + (slot !== null && slot !== undefined && slot.page === %d ? (slot.index %% k_nbrOfColumns) * k_itemWidth : 0)\n"
            "                    y: %s.y + (slot !== null && slot !== undefined && slot.page === %d ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)\n"
            "                    width: k_itemWidth\n"
            "                    height: k_itemHeight\n"
            "                    z: 7\n"
            "                    onClicked: wifi1bPage.open()\n"
            "                }\n", num, index, page, menu, page, menu, page);
        if (!insert_after_block(bufp, lenp, best_st, ins)) return false;
        s = *bufp;
    }
    return append_page(bufp, lenp);
}

static bool patch_wide(char **bufp, size_t *lenp)
{
    if (strstr(*bufp, "id: itemWifi1b")) return append_page(bufp, lenp);
    size_t best_end = 0, best_start = 0;
    const char *best_id = NULL;
    const char *ids[] = {"itemReboot", "itemTweaks", "itemWifi2", "itemWifi", "itemHello", NULL};
    for (int i = 0; ids[i]; i++) {
        size_t st, en;
        if (find_typed_id_block(*bufp, *lenp, "QHomeItem", ids[i], &st, &en) && en > best_end) {
            best_start = st; best_end = en; best_id = ids[i];
        }
    }
    const char *anchor = best_id ? best_id : "mainGridItemMenu";
    int width = 166;
    int icon_x = 42;
    if (best_id && !strcmp(best_id, "itemReboot")) {
        /* Same proven append pattern as the Tweaks anchor: a fourth compact
         * shortcut chained to the right of the previous custom tile. Do not
         * rewrite stock cell widths or icon offsets in the shared strip. */
        width = 150; icon_x = 38;
    } else if (best_id && !strcmp(best_id, "itemTweaks")) {
        width = 150; icon_x = 38;
    } else if (!best_id) {
        width = 200; icon_x = 50;
    } else {
        /* Stock/Hello Wide has 200px cells; shrink them for the extra tile. */
        char *p = strstr(*bufp, "200 : 207");
        while (p) {
            memcpy(p, "166 : 207", 9);
            p = strstr(p + 9, "200 : 207");
        }
    }
    char ins[1200];
    snprintf(ins, sizeof(ins),
        "\n        QHomeItem {\n"
        "            id: itemWifi1b\n"
        "            visible: AppHomeModel.isWideScr\n"
        "            width: AppHomeModel.isWideScr ? %d : 207\n"
        "            height: 183\n"
        "            x: %s.right\n"
        "            x_icon: AppHomeModel.isWideScr ? %d : 55\n"
        "            iconSource: [\"file:///app/share/AppHomeScreen/wifi1b/wifi1b_icon_n.png\", defineModel.home_bg_icon_p, \"\", \"file:///app/share/AppHomeScreen/wifi1b/wifi1b_icon_d.png\"]\n"
        "            itemName: \"WiFi 1b\"\n"
        "            icoPressed: wifi1bMouse.pressed\n"
        "            MouseArea {\n"
        "                id: wifi1bMouse\n"
        "                anchors.fill: parent\n"
        "                onClicked: wifi1bPage.open()\n"
        "            }\n"
        "        }\n", width, anchor, icon_x);
    size_t anchor_pos = best_start;
    if (!best_id) {
        const char *g = strstr(*bufp, "GridView");
        if (!g) return false;
        anchor_pos = (size_t)(g - *bufp);
    }
    if (!insert_after_block(bufp, lenp, anchor_pos, ins)) return false;
    return append_page(bufp, lenp);
}

static bool patch_menu(char **bufp, size_t *lenp)
{
    if (!strstr(*bufp, "onWifi1bClicked:")) {
        const char *p = strstr(*bufp, "onWifiClicked:");
        if (!p) p = strstr(*bufp, "onMenuItemClicked:");
        if (!p) return false;
        const char *nl = strchr(p, '\n');
        const char *brace = memchr(p, '{', nl ? (size_t)(nl - p) : strlen(p));
        if (brace) {
            if (!insert_after_block(bufp, lenp, (size_t)(p - *bufp),
                                    "\n            onWifi1bClicked: wifi1bPage.open()")) return false;
        } else {
            size_t at = nl ? (size_t)(nl - *bufp) + 1 : *lenp;
            if (!insert_at(bufp, lenp, at,
                           "            onWifi1bClicked: wifi1bPage.open()\n")) return false;
        }
    }
    return append_page(bufp, lenp);
}

static bool patch_custom(char **bufp, size_t *lenp)
{
    if (!strstr(*bufp, "signal wifi1bClicked()")) {
        /* WiFi-based layouts declare signal wifiClicked(); a standalone
         * install anchors on whichever custom signal Tweaks added. */
        static const char *anchors[] = {"signal wifiClicked()", "signal rebootClicked()",
                                        "signal tweaksClicked()", "signal helloClicked()",
                                        "signal batteryClicked()", NULL};
        const char *p = NULL;
        size_t alen = 0;
        for (int i = 0; anchors[i]; i++) {
            p = strstr(*bufp, anchors[i]);
            if (p) { alen = strlen(anchors[i]); break; }
        }
        if (!p) return false;
        const char *ins = "\n    signal wifi1bClicked()";
        size_t at = (size_t)(p - *bufp) + alen;
        char *nb = realloc(*bufp, *lenp + strlen(ins) + 2);
        if (!nb) return false;
        memmove(nb + at + strlen(ins), nb + at, *lenp - at + 1);
        memcpy(nb + at, ins, strlen(ins));
        *bufp = nb; *lenp += strlen(ins);
    }
    if (strstr(*bufp, "id: wifi1bTile")) return true;
    int skip = count_custom_tiles(*bufp, *lenp);
    size_t best_end = 0, best_start = 0;
    const char *types[] = {"HelloTile", "BatteryTile", "WifiTile", "TweaksTile", "RebootTile", "Wifi2Tile", NULL};
    for (int i = 0; types[i]; i++) {
        const char *p = *bufp;
        while ((p = strstr(p, types[i])) != NULL) {
            size_t st = (size_t)(p - *bufp);
            size_t en = find_block_end(*bufp, *lenp, st);
            if (en > best_end) { best_end = en; best_start = st; }
            p += strlen(types[i]);
        }
    }
    if (!best_end) return false;
    char ins[1200];
    snprintf(ins, sizeof(ins),
        "\n            Wifi1bTile {\n"
        "                id: wifi1bTile\n"
        "                property int emptyIdx: root.firstEmptyIndex(menuPage.pageModel, %d)\n"
        "                visible: (isAllMenuScreen || listType === AppHome_Enum.E_LIST_SETUP_MAIN) && !customizeMode && index === menuList.currentIndex && emptyIdx >= 0\n"
        "                x: menuPage.x + (emptyIdx %% k_nbrOfColumns) * k_itemWidth\n"
        "                y: menuPage.y + Math.floor(emptyIdx / k_nbrOfColumns) * k_itemHeight\n"
        "                width: k_itemWidth\n"
        "                height: k_itemHeight\n"
        "                z: 7\n"
        "                onClicked: root.wifi1bClicked()\n"
        "            }\n", skip);
    return insert_after_block(bufp, lenp, best_start, ins);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s advanced|wide|menu|custom FILE\n", argv[0]);
        return 2;
    }
    size_t len = 0;
    char *buf = read_all(argv[2], &len);
    if (!buf) { perror(argv[2]); return 1; }
    if (strstr(buf, "WIFI1B_PAYLOAD_260128")) {
        fprintf(stderr, "already patched: %s\n", argv[2]);
        free(buf); return 0;
    }
    if (!add_marker(&buf, &len)) { free(buf); return 1; }
    bool ok = false;
    if (!strcmp(argv[1], "advanced")) ok = patch_advanced(&buf, &len);
    else if (!strcmp(argv[1], "wide")) ok = patch_wide(&buf, &len);
    else if (!strcmp(argv[1], "menu")) ok = patch_menu(&buf, &len);
    else if (!strcmp(argv[1], "custom")) ok = patch_custom(&buf, &len);
    else { fprintf(stderr, "unknown mode: %s\n", argv[1]); free(buf); return 2; }
    if (!ok) { fprintf(stderr, "patch failed: %s %s\n", argv[1], argv[2]); free(buf); return 1; }
    if (write_atomic(argv[2], buf, len) != 0) { perror(argv[2]); free(buf); return 1; }
    free(buf);
    return 0;
}
