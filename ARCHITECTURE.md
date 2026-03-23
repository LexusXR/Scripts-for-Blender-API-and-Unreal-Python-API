# Архитектура плагина Blender To UE

## Общая схема

```
Blender                              Unreal Engine (Editor)
┌──────────────────────┐             ┌──────────────────────────────┐
│  BlenderLiveBridge   │   HTTP      │  BlenderToUEImporter module  │
│  Addon (Python)      │◄───────────►│  (C++)                       │
│                      │  :8765      │                              │
│  ┌────────────────┐  │             │  ┌────────────────────────┐  │
│  │ HTTP Server    │  │  POST       │  │ RequestRunningBlender  │  │
│  │ (threading)    │◄─┼─/export─────┼──│ Export (curl.exe)      │  │
│  │                │──┼─OK|paths───►│  │                        │  │
│  └────────────────┘  │             │  └───────────┬────────────┘  │
│  ┌────────────────┐  │             │              │               │
│  │ Task Queue     │  │             │  ┌───────────▼────────────┐  │
│  │ (main thread)  │  │             │  │ ImportFileToUnreal     │  │
│  └───────┬────────┘  │             │  │ (FBX → UAsset)         │  │
│  ┌───────▼────────┐  │             │  └───────────┬────────────┘  │
│  │ Export Pipeline │  │             │  ┌───────────▼────────────┐  │
│  │ (duplicate →   │  │             │  │ ApplyMasterMaterial    │  │
│  │  prepare →     │  │             │  │ Pipeline (textures →   │  │
│  │  FBX export)   │  │             │  │ material instances)    │  │
│  └────────────────┘  │             │  └────────────────────────┘  │
└──────────────────────┘             └──────────────────────────────┘
```

## Компоненты

### 1. Blender Addon — `BlenderPlugin/BlenderLiveBridgeAddon.py`

Один файл, 386 строк. Регистрируется как стандартный Blender-аддон.

#### HTTP-сервер

- `ThreadingHTTPServer` на `127.0.0.1:8765`, запускается в daemon-потоке.
- Два эндпоинта:
  - `GET /health` — возвращает `OK|alive` (используется UE для проверки соединения).
  - `POST /export_active` — принимает JSON с параметрами экспорта, ставит задачу в очередь.

#### Очередь задач и потокобезопасность

Blender API (`bpy.ops.*`) можно вызывать только из главного потока.
HTTP-запрос приходит в поток сервера, поэтому используется схема:

1. HTTP-поток создает `task` с `threading.Event` и кладет его в `_task_queue`.
2. HTTP-поток блокируется на `event.wait(timeout=180)`.
3. В главном потоке Blender работает таймер `_process_queue` (каждые 0.1 сек), который забирает задачу из очереди, выполняет экспорт и сигналит `event.set()`.
4. HTTP-поток просыпается и отправляет результат клиенту.

#### Автоопределение источника экспорта

Функция `_resolve_source_meshes()` — приоритетная цепочка:

1. **Collection Instance** — если активный объект это `EMPTY` с `instance_type == 'COLLECTION'`, берутся все меши из этой коллекции.
2. **Выделенные меши** — `bpy.context.selected_objects` с фильтром `type == "MESH"`.
3. **Активный меш** — `bpy.context.view_layer.objects.active`.

Если `export_mode == "active_only"` и найдено больше одного меша, режим автоматически повышается до `merge_selected`.

#### Экспортный пайплайн

Функция `_export_from_scene(output_path, apply_modifiers, auto_uv, export_mode)`:

1. Вызывает `_resolve_source_meshes()` для получения набора мешей.
2. Дублирует меши через `bpy.data` API (не через `bpy.ops`) для стабильности.
3. На каждом дубликате выполняет `_prepare_mesh`:
   - Apply transforms (location, rotation, scale).
   - Apply modifiers (если включено в настройках).
   - Auto UV через `smart_project` (если нет UV-слоев и включено в настройках).
   - Center origin (`ORIGIN_GEOMETRY`, `BOUNDS`) — гизмо переносится в центр bounding box.
4. Экспортирует в FBX с embedded текстурами (`path_mode="COPY"`, `embed_textures=True`).
5. Удаляет дубликаты из сцены.

Три режима экспорта:

| Режим | Поведение |
|---|---|
| `active_only` | Один FBX; если источник содержит несколько мешей — автоматически merge |
| `merge_selected` | Все меши объединяются (`bpy.ops.object.join`) в один FBX, origin по центру |
| `batch_selected` | Каждый меш экспортируется в отдельный FBX |

#### UI в Blender

Панель `View3D > Sidebar > BlenderToUE` с двумя кнопками (Start/Stop) и строкой статуса. Сервер автоматически стартует при `register()`.

---

### 2. Unreal Plugin — `UnrealPlugin/BlenderToUEImporter/`

Editor-only C++ модуль (`Type: Editor`). Зависимости: `AssetTools`, `AssetRegistry`, `CoreUObject`, `DeveloperSettings`, `Engine`, `Slate`, `SlateCore`, `ToolMenus`, `UnrealEd`.

#### Структура файлов

```
BlenderToUEImporter/
├── BlenderToUEImporter.uplugin          # манифест плагина
└── Source/BlenderToUEImporter/
    ├── BlenderToUEImporter.Build.cs     # зависимости сборки
    ├── Public/
    │   ├── BlenderToUEImporterModule.h      # интерфейс модуля
    │   └── BlenderToUEImporterSettings.h    # UDeveloperSettings (UPROPERTY)
    └── Private/
        └── BlenderToUEImporterModule.cpp    # реализация
```

#### Модуль `FBlenderToUEImporterModule`

| Метод | Назначение |
|---|---|
| `StartupModule` / `ShutdownModule` | Регистрация/снятие меню через `UToolMenus` |
| `RegisterMenus` | Добавляет в `Tools`: кнопку импорта + динамический индикатор статуса |
| `ImportFromRunningBlender` | Точка входа: запрос к bridge → импорт FBX |
| `RequestRunningBlenderExport` | Отправляет `POST /export_active` через `curl.exe`, парсит ответ `OK|path1|path2...` |
| `IsRunningBlenderBridgeAvailable` | `GET /health` через `curl.exe` с таймаутом 2 сек |
| `ImportFileToUnreal` | Создает `UAssetImportTask` с настройками FBX-импорта, вызывает `AssetTools::ImportAssetTasks` |
| `ApplyMasterMaterialPipeline` | Пост-импортная обработка материалов |

#### Настройки `UBlenderToUEImporterSettings`

`UDeveloperSettings` с `Config=EditorPerProjectUserSettings`. Доступны в `Project Settings > Plugins > Blender To UE Importer`.

Категории настроек:

- **Import** — `DestinationRoot` (куда складывать ассеты).
- **Blender Pipeline** — `bApplyModifiers`, `bAutoGenerateUvIfMissing`, `ExportMode`.
- **Unreal Import** — `bImportMaterials`, `bImportTextures`.
- **Master Material** — `MasterMaterial`, `bCreateMaterialInstancePerSlot`, `bUsePackedORMTexture`, имена параметров текстур.
- **Texture Matching** — wildcard-паттерны для сопоставления текстур по семантике.

#### Material Pipeline

Работает только если задан `MasterMaterial`. Последовательность:

1. Собирает все `UTexture` из папки модели через `AssetRegistry`.
2. Классифицирует каждую текстуру по семантике (`BaseColor`, `Normal`, `Roughness`, `Metallic`, `AO`, `ORM`) — сначала по пользовательским wildcard-паттернам, затем по встроенной эвристике имён.
3. Группирует текстуры в бакеты: глобальный + по ключу материала (имя текстуры без суффикса типа).
4. Сопоставляет бакет с каждым material slot меша через нормализацию имён.
5. Создает `UMaterialInstanceConstant` (из `MasterMaterial`) и назначает текстуры на параметры.

Два режима назначения:

- `bCreateMaterialInstancePerSlot = true` → `MI_<Model>_<Slot>_Auto` на каждый слот.
- `bCreateMaterialInstancePerSlot = false` → один `MI_<Model>_Auto` на все слоты.

---

## Протокол взаимодействия

### Health Check

```
UE → GET http://127.0.0.1:8765/health
UE ← 200 "OK|alive"
```

### Export Request

```
UE → POST http://127.0.0.1:8765/export_active
     Content-Type: application/json
     {
       "output_path": "C:\\...\\LiveBlend_20260319_120000.fbx",
       "apply_modifiers": true,
       "auto_uv": true,
       "export_mode": "active_only"
     }

UE ← 200 "OK|C:\\...\\LiveBlend_20260319_120000.fbx"
     или
UE ← 200 "OK|path1.fbx|path2.fbx"  (batch mode)
     или
UE ← 500 "ERROR|описание ошибки"
UE ← 504 "ERROR|timeout"
```

### Формат ответа

Все ответы — `text/plain`. Первый токен до `|` это статус (`OK` или `ERROR`), остальное — payload.

---

## Ограничения

- **Windows only** — коммуникация UE ↔ bridge через `curl.exe`.
- **Только StaticMesh** — skeletal mesh не поддерживается.
- **Синхронный вызов** — UE блокирует главный поток на время `curl.exe` + экспорта (до 180 сек).
- **Один запрос** — очередь в Blender последовательная, параллельные запросы ждут.
