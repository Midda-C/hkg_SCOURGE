#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: AppHomeScreen eco3.0, Gen5W_L EU 260128
"""Normalize first-party Advanced tiles; never patch arbitrary installed QML."""
import re

ORDER = ('hello', 'battery', 'wifi', 'tweaks', 'reboot', 'wifi1b')

HELPERS = '''    // SCOURGE_LAYOUT_V2: one reactive allocation snapshot for every tile.
    property bool customLayoutReady: false
    property int customLayoutRevision: 0
    property var customLayoutSlots: {
        var revision = customLayoutRevision
        return customLayoutReady ? computeCustomLayout() : []
    }

    function emptySlots(pageModel) {
        if (!pageModel) return null
        var n = typeof pageModel.getCount === "function" ? pageModel.getCount() : pageModel.count
        if (typeof n !== "number" || n < 0 || n > nbrItemsPerPage || Math.floor(n) !== n) return null
        if (n > 0 && typeof pageModel.getItem !== "function") return null
        var slots = []
        for (var i = 0; i < n; i++) {
            var item = pageModel.getItem(i)
            if (!item) return null
            var occupied
            if (typeof item.isEditingVisible === "function") occupied = item.isEditingVisible()
            else if (typeof item.visible === "boolean") occupied = item.visible
            else return null
            if (typeof occupied !== "boolean") return null
            if (!occupied) slots.push(i)
        }
        for (var j = n; j < nbrItemsPerPage; j++) slots.push(j)
        return slots
    }

    function computeCustomLayout() {
        var result = []
        for (var p = 0; p < totalPagesNormalMode; p++) {
            var slots = emptySlots(getPageDisplayListModel(p))
            if (slots === null) return []
            for (var i = 0; i < slots.length; i++) result.push({page: p, index: slots[i]})
        }
        return result
    }

    function firstEmptySlot(customIndex) {
        return customIndex >= 0 && customIndex < customLayoutSlots.length ? customLayoutSlots[customIndex] : null
    }

    function firstEmptyIndex(pageModel, skip) {
        if (skip === undefined) skip = 0
        var slots = emptySlots(pageModel)
        return slots !== null && skip < slots.length ? slots[skip] : -1
    }
'''


def block_end(text, start):
    """Find braces while respecting quoted strings and comments."""
    token = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for match in token.finditer(text, text.index('{', start)):
        if match[0] == '{':
            depth += 1
        elif match[0] == '}':
            depth -= 1
            if depth == 0:
                return match.end()
    raise ValueError('unbalanced QML block')


def normalize(text):
    # Only build-time, known first-party source variants enter this function.
    # Existing tiles are rewritten, not accepted because an id/helper exists.
    pattern = re.compile(r'(?m)^([ \t]*)(Hello|Battery|Wifi|Tweaks|Reboot|Wifi1b)Tile\s*\{\s*id:\s*(hello|battery|wifi|tweaks|reboot|wifi1b)Tile([123])\b')
    matches = list(pattern.finditer(text))
    if not matches:
        return text
    names = {m[3] for m in matches}
    ordinals = {name: i for i, name in enumerate(n for n in ORDER if n in names)}
    seen = set()
    for m in reversed(matches):
        name, page = m[3], int(m[4])
        if (name, page) in seen:
            raise ValueError(f'duplicate {name}Tile{page}')
        seen.add((name, page))
        end = block_end(text, m.start())
        body = text[m.start():end]
        click = re.search(r'^\s*(onClicked:.*)$', body, re.M)
        if not click:
            raise ValueError(f'missing click handler: {name}Tile{page}')
        menu = 'menuPage' if page == 1 else f'menuPage{page}'
        condition = f'slot !== null && slot !== undefined && slot.page === {page - 1}'
        indent = m[1]
        lines = [f'{m[2]}Tile {{', f'    id: {name}Tile{page}',
                 f'    property var slot: root.firstEmptySlot({ordinals[name]})',
                 f'    visible: {condition}',
                 f'    x: {menu}.x + ({condition} ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)',
                 f'    y: {menu}.y + ({condition} ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)',
                 '    width: k_itemWidth', '    height: k_itemHeight', '    z: 7',
                 '    ' + click[1], '}']
        text = text[:m.start()] + '\n'.join(indent + line for line in lines) + text[end:]
    for name in names:
        if any((name, page) not in seen for page in (1, 2, 3)):
            raise ValueError(f'incomplete page instances: {name}')
    for function in ('emptySlots', 'firstEmptySlot', 'firstEmptyIndex', 'tweaksTargetPage'):
        while True:
            match = re.search(r'(?m)^    function ' + function + r'\([^)]*\)\s*\{', text)
            if not match:
                break
            end = block_end(text, match.start())
            text = text[:match.start()] + text[end:]
    # Remove the previous version's root properties on repeated normalization.
    text = re.sub(r'(?m)^    // SCOURGE_LAYOUT_V2[^\n]*\n', '', text)
    text = re.sub(r'(?m)^    property (?:bool customLayoutReady|int customLayoutRevision):[^\n]*\n', '', text)
    prop = re.search(r'(?m)^    property var customLayoutSlots:\s*\{', text)
    if prop:
        end = block_end(text, prop.start())
        text = text[:prop.start()] + text[end:]
    match = re.search(r'(?m)^    function computeCustomLayout\([^)]*\)\s*\{', text)
    if match:
        end = block_end(text, match.start())
        text = text[:match.start()] + text[end:]
    anchor = '    function getPageDisplayListModel(index)'
    if text.count(anchor) != 1:
        raise ValueError('missing global model accessor')
    text = text.replace(anchor, HELPERS + '\n' + anchor, 1)
    # Explicit refresh after C++ mutation: method calls do not necessarily
    # establish QML property-notification dependencies when count is unchanged.
    if '        customLayoutReady = false\n' not in text:
        text = text.replace('    function constructMenuModel(){', '    function constructMenuModel(){\n        customLayoutReady = false', 1)
        anchor = '        updateDisplayList()\n'
        if anchor not in text:
            raise ValueError('missing model rebuild hook')
        text = text.replace(anchor, anchor + '        customLayoutRevision++\n        customLayoutReady = true\n', 1)
        anchor = '        AppHomeModel.updateItemVisible(itemId, visible)'
        if anchor in text:
            text = text.replace(anchor, anchor + '\n        customLayoutRevision++', 1)
    return text


if __name__ == '__main__':
    import sys
    from pathlib import Path
    for name in sys.argv[1:]:
        path = Path(name)
        path.write_text(normalize(path.read_text()))
