#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: AppHomeScreen eco3.0, Gen5W_L EU 260128
"""Exercise packed placement bindings AND actual tile MouseAreas in Qt Quick.

Vendor C++ models/theme/beep services are fixtures, not production equivalence.
--emit DIR retains a Qt5-compatible fixture for execution in the vendor VM.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from qml_layout import block_end

packages = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('qml', type=Path)
parser.add_argument('--emit', type=Path)
args = parser.parse_args()


def emit(dest):
    dest.mkdir(parents=True, exist_ok=True)
    text = args.qml.read_text()
    tile_types = ['Battery']
    if re.search(r'\bWifiTile\s*\{', text):
        tile_types.append('Wifi')
    tile_types.extend(['Tweaks', 'Reboot'])
    if 'id: wifi1bTile1' in text:
        tile_types.append('Wifi1b')
    start = text.index('    // SCOURGE_LAYOUT_V2')
    helpers = text[start:text.index('    function getPageDisplayListModel(index)', start)]
    tiles = []
    for page in (1, 2, 3):
        menu = 'menuPage' if page == 1 else f'menuPage{page}'
        blocks = []
        for tile in tile_types:
            m = re.search(tile + r'Tile\s*\{\s*id:\s*' + tile.lower() + 'Tile' + str(page) + r'\b', text)
            assert m, (tile, page)
            block = text[m.start():block_end(text, m.start())]
            # The application-specific action is outside the placement test;
            # retain the actual component's MouseArea and clicked() signal.
            block = re.sub(r'onClicked:.*', 'onClicked: root.clicks++', block)
            block = block.replace('property var slot:', f'objectName: "{tile.lower()}{page}"\nproperty var slot:')
            blocks.append(block)
        tiles.append(f'Item {{ x: {page - 1} * 1920; width: 1920; height: 720\n'
                     f'Item {{ id: {menu}; x: 42; y: 135 }}\n' + '\n'.join(blocks) + '\n}')
    for tile in tile_types:
        app = tile.lower() if tile in ('Battery', 'Wifi', 'Wifi1b') else 'tweaks'
        source = packages / app / f'artifacts/qml/AppHomeScreen/qml_eco3.0/component/{tile}Tile.qml'
        component = source.read_text()
        component = re.sub(r'^import (?:HMIEVENTS|AppHome_Enum).*\n', '', component, flags=re.M)
        component = component.replace('ConstantModel.FONT_SIZE_39', '39')
        component = component.replace('QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_HOME_SOUND_BEEP_PLAY)', 'root.beepForTest()')
        component = component.replace('id: root', 'id: root\n    function beepForTest() {}', 1)
        component = re.sub(r'file:///app/share/AppHomeScreen/[^"\n]+', '', component)
        (dest / f'{tile}Tile.qml').write_text(component)
    (dest / 'common').mkdir(exist_ok=True)
    (dest / 'common/QText.qml').write_text('import QtQuick 2.3\nText {}\n')
    (dest / 'common/Q_ThemeImage.qml').write_text('import QtQuick 2.3\nRectangle { property string themeSource; width: 128; height: 128; color: "#aaaaaa" }\n')
    scene = '''import QtQuick 2.3
import QtTest 1.0
Item {
 id: root; width: 5760; height: 720
 property int k_nbrOfColumns: 6
 property int k_itemWidth: 306
 property int k_itemHeight: 249
 property int nbrItemsPerPage: k_nbrOfColumns * 2
 property int totalPagesNormalMode: 3
 property int clicks: 0
 property var testPages: []
 QtObject { id: defineModel; property string home_bg_icon_n: ""; property string home_bg_icon_p: "" }
 function getPageDisplayListModel(p) { return testPages[p] }
 function model(states) { return {getCount: function(){return states.length}, getItem: function(i){return states[i] === null ? null : {isEditingVisible:function(){return states[i]}}}} }
 function all(occupied) { var a=[]; for(var i=0;i<nbrItemsPerPage;i++) a.push(occupied); return a }
 function installModels(pages) { customLayoutReady=false; testPages=pages; customLayoutRevision++; customLayoutReady=true }
 HELPERS
 TILES
 TestCase {
  name: "PackedLayout"; when: windowShown
  function test_layout_data() { return [{tag:"wide",cols:6},{tag:"narrow",cols:4}] }
  function test_layout(data) {
   root.k_nbrOfColumns=data.cols
   var first=root.all(true), second=root.all(false), third=root.all(false)
   second[0]=true; second[1]=true; second[2]=true
   root.customLayoutReady=false
   wait(50)
   verifyCount(0)
   root.installModels([root.model(first),root.model(second),root.model(third)])
   wait(50)
    verifyCount(TILE_COUNT)
   verifyPositions()
    for(var t=0;t<TILE_COUNT;t++) for(var p=1;p<=3;p++) {
     var tile=findChild(root,TILE_NAMES[t]+p)
    if(!tile.visible) continue
    var n=root.clicks
    mouseClick(tile,tile.width/2,tile.height/2)
    compare(root.clicks,n+1)
   }
    // Only two cells on page two: remaining custom tiles must spill to page three.
    var limited=root.all(true); limited[0]=false; limited[1]=false
    root.installModels([root.model(first),root.model(limited),root.model(third)])
    wait(50); verifyCount(TILE_COUNT); verifyPositions()
    // Same count, changed occupancy: explicit model-rebuild revision must move tiles.
   first[0]=false; second[3]=true
   root.installModels([root.model(first),root.model(second),root.model(third)])
   wait(50)
    verifyCount(TILE_COUNT); verifyPositions()
   verify(findChild(root,"battery1").visible)
   var battery=findChild(root,"battery1"), before=root.clicks
   mouseClick(battery,battery.width/2,battery.height/2)
   compare(root.clicks,before+1)
   root.installModels([root.model(root.all(true)),root.model(root.all(true)),root.model(root.all(true))])
   wait(50); verifyCount(0)
   root.installModels([null,root.model(second),root.model(third)])
   wait(50); verifyCount(0)
   var unknown=root.all(true); unknown[0]=null
   root.installModels([root.model(unknown),root.model(second),root.model(third)])
   wait(50); verifyCount(0)
  }
  function verifyCount(expected) {
   var count=0
    for(var p=1;p<=3;p++) for(var t=0;t<TILE_COUNT;t++) {
     var tile=findChild(root,TILE_NAMES[t]+p)
    verify(tile!==null)
    if(tile.visible) count++
   }
   compare(count,expected)
  }
  function verifyPositions() {
   var used={}
    for(var p=1;p<=3;p++) for(var t=0;t<TILE_COUNT;t++) {
     var tile=findChild(root,TILE_NAMES[t]+p)
    if(!tile.visible) continue
    compare(tile.slot.page,p-1)
    verify(!used[p+":"+tile.slot.index]); used[p+":"+tile.slot.index]=true
    verify(!root.testPages[p-1].getItem(tile.slot.index).isEditingVisible())
    compare(tile.x,42+(tile.slot.index%root.k_nbrOfColumns)*306)
    compare(tile.y,135+Math.floor(tile.slot.index/root.k_nbrOfColumns)*249)
   }
  }
 }
}
'''.replace(' HELPERS', helpers).replace(' TILES', '\n'.join(tiles))
    import json
    scene = scene.replace('TILE_COUNT', str(len(tile_types))).replace('TILE_NAMES', json.dumps([t.lower() for t in tile_types]))
    (dest / 'tst_layout.qml').write_text(scene)


if args.emit:
    emit(args.emit)
    print('Emitted Qt5-compatible runtime fixture')
else:
    runner = shutil.which('qmltestrunner') or '/usr/lib/qt6/bin/qmltestrunner'
    if not Path(runner).is_file():
        raise SystemExit('Qt Quick Test runner required; this gate must not silently skip')
    with tempfile.TemporaryDirectory(prefix='qml-layout-', dir='/tmp/opencode') as d:
        emit(Path(d))
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software', QSG_RHI_BACKEND='software')
        run = subprocess.run([runner, '-input', d], env=env, text=True, capture_output=True, timeout=60)
        print(run.stdout)
        if run.returncode or any(word in run.stderr for word in ('ReferenceError', 'TypeError', 'Binding loop', 'is not defined')):
            raise SystemExit(run.stderr or f'Qt tests failed: {run.returncode}')
        print('PASS: Qt Quick bindings, initialization, same-count rearrangement, overflow, and actual tile click')
