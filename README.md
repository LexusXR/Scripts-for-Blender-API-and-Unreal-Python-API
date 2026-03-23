# Blender to UE: Unreal + Blender plugins

В этой папке лежат оба плагина:

- Unreal Engine plugin: `UnrealPlugin/BlenderToUEImporter`
- Blender addon: `BlenderPlugin/BlenderLiveBridgeAddon.py`

## 1) Установка Unreal plugin

1. Скопируй папку `BlenderToUEImporter` в:
   - `<YourProject>/Plugins/BlenderToUEImporter`
2. Перезапусти проект в Unreal.
3. Если появится запрос на сборку C++ модулей — подтверди.
4. Убедись, что плагин включен:
   - `Edit -> Plugins -> Blender To UE Importer`

## 2) Установка Blender addon (Live Bridge)

1. Открой Blender.
2. `Edit -> Preferences -> Add-ons -> Install...`
3. Выбери файл:
   - `BlenderPlugin/BlenderLiveBridgeAddon.py`
4. Включи аддон `Blender To UE Live Bridge`.
5. В 3D View открой боковую панель `N` -> вкладка `BlenderToUE`.
6. Нажми `Start Bridge Server` (обычно сервер стартует автоматически при включении аддона).

## 3) Использование в Unreal

В меню `Tools` доступны команды:

- `Import From Running Blender` — экспорт мешей из live Blender через bridge (источник определяется автоматически).

Также есть статус-строка:

- `Blender Live Bridge: Connected/Disconnected`.

Источник экспорта определяется автоматически в Blender:

- активный collection instance (если выбран),
- иначе выделенные меши,
- иначе активный меш.

## 4) Настройки плагина в Unreal

`Project Settings -> Plugins -> Blender To UE Importer`

- `DestinationRoot`
- `bApplyModifiers`
- `bAutoGenerateUvIfMissing`
- `ExportMode` (`Active Mesh Only` / `Merge Selected Meshes` / `Batch Export Selected Meshes`)
- `bImportMaterials`
- `bImportTextures`
- `MasterMaterial`
- `bCreateMaterialInstancePerSlot`
- `bUsePackedORMTexture`
- parameter names (`BaseColorParameterName`, `NormalParameterName`, `RoughnessParameterName`, `MetallicParameterName`, `AmbientOcclusionParameterName`, `ORMParameterName`)
- texture matching patterns (`BaseColorPatterns`, `NormalPatterns`, `RoughnessPatterns`, `MetallicPatterns`, `AmbientOcclusionPatterns`, `ORMPatterns`)

Если задан `MasterMaterial`, плагин создаёт material instances и сопоставляет текстуры по маскам/эвристике:

- по слотам: `MI_<ModelName>_<Slot>_Auto` (если `bCreateMaterialInstancePerSlot=true`)
- общий: `MI_<ModelName>_Auto` (если `bCreateMaterialInstancePerSlot=false`)
- поддержка BaseColor, Normal, Roughness, Metallic, AO и packed ORM

## 5) Почему статус Disconnected

`Disconnected` означает, что недоступен endpoint:

- `http://127.0.0.1:8765/health`

Проверь:

1. Blender действительно запущен.
2. Аддон `Blender To UE Live Bridge` включен.
3. В панели `BlenderToUE` нажата кнопка `Start Bridge Server`.
4. Порт `8765` не занят/не блокируется.

## 6) Быстрый smoke-test

1. В Blender сделай активным любой mesh.
2. В Unreal нажми `Tools -> Import From Running Blender`.
3. Проверь, что ассет появился в `DestinationRoot/<ModelName>`.
