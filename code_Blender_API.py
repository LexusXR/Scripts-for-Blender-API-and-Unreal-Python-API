import bpy
import bmesh
import os

# Получаем активный объект
obj = bpy.context.object
if obj is None or obj.type != 'MESH':
    raise TypeError("Выбранный объект должен быть типа MESH")
# Возьмём активные объекты, изменим их параметры
for obj in bpy.context.selected_objects:
    obj.location = (0, 0, 0)
    obj.scale = (1, 1, 1)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

mesh = obj.data

# Имя модели (из Blender)
model_name = obj.name
model_filename = f"{model_name}.fbx"


# Создаем bmesh и UV-развертку
bm = bmesh.new()
bm.from_mesh(mesh)

uv_layer = bm.loops.layers.uv.verify()

# Применяем UV-развертку
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.unwrap(method='ANGLE_BASED')
bpy.ops.object.mode_set(mode='OBJECT')

bm.to_mesh(mesh)
bm.free()

# Определяем путь для экспорта
documents_path = os.path.join(os.path.expanduser("~"), "Documents")
export_folder = os.path.join(documents_path, "exported_models")

if not os.path.exists(export_folder):
    os.makedirs(export_folder)

# Путь к файлу FBX
export_path = os.path.join(export_folder, model_filename)

# Экспортируем в FBX
bpy.ops.export_scene.fbx(
    filepath=export_path,
    use_selection=True,
    embed_textures=True,  # Встраивание текстур
    bake_space_transform=True,  # Корректное положение модели
    path_mode='COPY'  # Копируем текстуры внутрь FBX
)


