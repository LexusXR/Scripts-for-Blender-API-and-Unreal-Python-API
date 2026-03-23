bl_info = {
    "name": "Blender To UE Live Bridge",
    "author": "BlenderToUEImporter",
    "version": (1, 2, 0),
    "blender": (3, 0, 0),
    "location": "View3D > Sidebar > BlenderToUE",
    "description": "Exposes local HTTP endpoint for exporting meshes to UE",
    "category": "Import-Export",
}

import contextlib
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import bmesh
import bpy
from mathutils import Matrix, Vector


_server = None
_server_thread = None
_task_queue = []
_queue_lock = threading.Lock()
_last_server_error = ""


@contextlib.contextmanager
def _view3d_override():
    ctx = {}
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                for region in area.regions:
                    if region.type == "WINDOW":
                        ctx = {"window": window, "area": area, "region": region}
                        break
                if ctx:
                    break
        if ctx:
            break
    if ctx and hasattr(bpy.context, "temp_override"):
        with bpy.context.temp_override(**ctx):
            yield
    else:
        yield


def _deselect_all():
    for obj in bpy.context.view_layer.objects:
        obj.select_set(False)


def _set_active_and_select(obj):
    _deselect_all()
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def _selected_meshes():
    return [
        (obj, obj.matrix_world.copy())
        for obj in bpy.context.selected_objects
        if obj.type == "MESH" and obj.data is not None
    ]


def _active_collection_instance_meshes():
    active = bpy.context.view_layer.objects.active
    if active is None or active.type != "EMPTY":
        return []
    if active.instance_type != "COLLECTION" or active.instance_collection is None:
        return []

    target_collection = active.instance_collection
    collection_obj_set = set(target_collection.all_objects)

    result = []
    depsgraph = bpy.context.evaluated_depsgraph_get()
    for instance in depsgraph.object_instances:
        if not instance.is_instance:
            continue
        if instance.parent is None or instance.parent.original != active:
            continue
        obj_orig = instance.object.original
        if obj_orig not in collection_obj_set:
            continue
        if obj_orig.type != "MESH" or obj_orig.data is None:
            continue
        result.append((obj_orig, instance.matrix_world.copy()))

    if not result:
        instance_matrix = active.matrix_world.copy()
        offset = Vector(target_collection.instance_offset)
        offset_inv = Matrix.Translation(-offset)
        result = [
            (obj, instance_matrix @ offset_inv @ obj.matrix_world)
            for obj in target_collection.all_objects
            if obj.type == "MESH" and obj.data is not None
        ]

    return result


def _resolve_source_meshes():
    meshes = _active_collection_instance_meshes()
    if meshes:
        return meshes

    meshes = _selected_meshes()
    if meshes:
        return meshes

    active = bpy.context.view_layer.objects.active
    if active is not None and active.type == "MESH" and active.data is not None:
        return [(active, active.matrix_world.copy())]

    raise RuntimeError("No mesh source found (active mesh, selected meshes, or active collection instance)")


def _apply_transforms(obj):
    if obj.data is None:
        return
    world_matrix = obj.matrix_world.copy()
    obj.data.transform(world_matrix)
    if hasattr(obj.data, "calc_normals"):
        obj.data.calc_normals()
    obj.data.update()
    if obj.parent is not None:
        obj.parent = None
        obj.matrix_parent_inverse = Matrix.Identity(4)
    obj.matrix_world = Matrix.Identity(4)


def _apply_modifiers(obj):
    if not obj.modifiers:
        return
    original_materials = [slot.material for slot in obj.material_slots]
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    eval_obj = obj.evaluated_get(depsgraph)
    new_mesh = bpy.data.meshes.new_from_object(
        eval_obj,
        preserve_all_data_layers=True,
        depsgraph=depsgraph,
    )
    while len(new_mesh.materials) > len(original_materials):
        new_mesh.materials.pop()
    for i, mat in enumerate(original_materials):
        if i < len(new_mesh.materials):
            new_mesh.materials[i] = mat
        else:
            new_mesh.materials.append(mat)
    old_mesh = obj.data
    obj.data = new_mesh
    obj.modifiers.clear()
    if old_mesh and old_mesh.users == 0:
        bpy.data.meshes.remove(old_mesh)


def _ensure_uv(obj):
    if obj.data is None or len(obj.data.uv_layers) > 0:
        return
    bm_uv = bmesh.new()
    bm_uv.from_mesh(obj.data)
    uv_layer = bm_uv.loops.layers.uv.new("UVMap")
    for face in bm_uv.faces:
        normal = face.normal
        abs_n = Vector((abs(normal.x), abs(normal.y), abs(normal.z)))
        for loop in face.loops:
            co = loop.vert.co
            if abs_n.x >= abs_n.y and abs_n.x >= abs_n.z:
                loop[uv_layer].uv = (co.y, co.z)
            elif abs_n.y >= abs_n.x and abs_n.y >= abs_n.z:
                loop[uv_layer].uv = (co.x, co.z)
            else:
                loop[uv_layer].uv = (co.x, co.y)
    bm_uv.to_mesh(obj.data)
    bm_uv.free()
    obj.data.update()


def _center_origin(obj):
    if obj.data is None or not obj.data.vertices:
        return
    verts = obj.data.vertices
    min_co = Vector(verts[0].co)
    max_co = Vector(verts[0].co)
    for v in verts[1:]:
        for i in range(3):
            if v.co[i] < min_co[i]:
                min_co[i] = v.co[i]
            if v.co[i] > max_co[i]:
                max_co[i] = v.co[i]
    center = (min_co + max_co) / 2.0
    for v in verts:
        v.co -= center
    obj.data.update()


def _fill_empty_material_slots(obj):
    for i in range(len(obj.data.materials)):
        if obj.data.materials[i] is None:
            placeholder = bpy.data.materials.new(name=f"Slot_{i}")
            placeholder.use_nodes = True
            obj.data.materials[i] = placeholder


def _export_fbx(obj, output_path):
    _fill_empty_material_slots(obj)
    _set_active_and_select(obj)
    with _view3d_override():
        bpy.ops.export_scene.fbx(
            filepath=output_path,
            use_selection=True,
            global_scale=1.0,
            apply_unit_scale=True,
            bake_space_transform=False,
            mesh_smooth_type="FACE",
            path_mode="COPY",
            embed_textures=True,
            add_leaf_bones=False,
            object_types={"MESH"},
        )


def _duplicate_meshes(meshes):
    duplicates = []
    scene_collection = bpy.context.scene.collection
    for obj, world_matrix in meshes:
        if obj is None or obj.type != "MESH" or obj.data is None:
            continue
        duplicate = obj.copy()
        duplicate.data = obj.data.copy()
        duplicate.animation_data_clear()
        scene_collection.objects.link(duplicate)
        duplicate.parent = None
        duplicate.matrix_parent_inverse = Matrix.Identity(4)
        duplicate.matrix_world = world_matrix.copy()

        for i, slot in enumerate(obj.material_slots):
            effective_mat = slot.material
            if i < len(duplicate.data.materials):
                duplicate.data.materials[i] = effective_mat

        duplicates.append(duplicate)
    if not duplicates:
        raise RuntimeError("No mesh objects available for duplication")
    return duplicates


def _cleanup_objects(objects):
    if not objects:
        return
    for obj in objects:
        if obj is None or obj.name not in bpy.data.objects:
            continue
        mesh_data = obj.data if obj.type == "MESH" else None
        bpy.data.objects.remove(obj, do_unlink=True)
        if mesh_data is not None and mesh_data.users == 0:
            bpy.data.meshes.remove(mesh_data)


def _join_meshes(objects):
    if not objects:
        raise RuntimeError("No objects to join")
    if len(objects) == 1:
        return objects[0]

    target = objects[0]
    all_materials = list(target.data.materials)

    combined = bmesh.new()
    combined.from_mesh(target.data)

    for obj in objects[1:]:
        mat_remap = {}
        for src_idx, mat in enumerate(obj.data.materials):
            found_idx = None
            for j, existing in enumerate(all_materials):
                if mat is existing:
                    found_idx = j
                    break
            if found_idx is not None:
                mat_remap[src_idx] = found_idx
            else:
                mat_remap[src_idx] = len(all_materials)
                all_materials.append(mat)

        face_offset = len(combined.faces)
        combined.from_mesh(obj.data)
        combined.verts.ensure_lookup_table()
        combined.faces.ensure_lookup_table()

        if mat_remap:
            for f in combined.faces[face_offset:]:
                f.material_index = mat_remap.get(f.material_index, 0)

    new_mesh = bpy.data.meshes.new(target.data.name + "_merged")
    combined.to_mesh(new_mesh)
    combined.free()
    new_mesh.update()

    for mat in all_materials:
        new_mesh.materials.append(mat)

    old_mesh = target.data
    target.data = new_mesh
    if old_mesh and old_mesh.users == 0:
        bpy.data.meshes.remove(old_mesh)

    return target


def _prepare_mesh(obj, apply_modifiers, auto_uv, center=True):
    if apply_modifiers:
        _apply_modifiers(obj)
    _apply_transforms(obj)
    if auto_uv:
        _ensure_uv(obj)
    if center:
        _center_origin(obj)


def _sanitize_name(name):
    invalid_chars = '<>:"/\\|?*'
    cleaned = "".join("_" if c in invalid_chars else c for c in name).strip()
    return cleaned if cleaned else "Mesh"


def _export_from_scene(output_path, apply_modifiers, auto_uv, export_mode):
    output_path = os.path.abspath(output_path)
    output_dir = os.path.dirname(output_path)
    base_name = os.path.splitext(os.path.basename(output_path))[0]
    os.makedirs(output_dir, exist_ok=True)

    exported_paths = []
    source_meshes = _resolve_source_meshes()

    if export_mode == "active_only" and len(source_meshes) > 1:
        export_mode = "merge_selected"

    if export_mode == "batch_selected":
        duplicates = _duplicate_meshes(source_meshes)
        try:
            for obj in duplicates:
                _prepare_mesh(obj, apply_modifiers, auto_uv)
                mesh_name = _sanitize_name(obj.name)
                mesh_path = os.path.join(output_dir, f"{base_name}_{mesh_name}.fbx")
                _export_fbx(obj, mesh_path)
                exported_paths.append(mesh_path)
        finally:
            _cleanup_objects(duplicates)

    elif export_mode == "merge_selected":
        duplicates = _duplicate_meshes(source_meshes)
        try:
            for obj in duplicates:
                _prepare_mesh(obj, apply_modifiers, auto_uv, center=False)
            merged = _join_meshes(duplicates)
            _center_origin(merged)
            _export_fbx(merged, output_path)
            exported_paths.append(output_path)
        finally:
            remaining = [o for o in duplicates if o and o.name in bpy.data.objects]
            _cleanup_objects(remaining)

    else:
        duplicate = _duplicate_meshes(source_meshes)[0]
        try:
            _prepare_mesh(duplicate, apply_modifiers, auto_uv)
            _export_fbx(duplicate, output_path)
            exported_paths.append(output_path)
        finally:
            _cleanup_objects([duplicate])

    if not exported_paths:
        raise RuntimeError("No files were exported")
    return exported_paths


def _process_queue():
    task = None
    with _queue_lock:
        if _task_queue:
            task = _task_queue.pop(0)

    if task is not None:
        payload = task["payload"]
        event = task["event"]
        try:
            export_mode = str(payload.get("export_mode", "active_only")).strip().lower()
            output_paths = _export_from_scene(
                payload["output_path"],
                bool(payload.get("apply_modifiers", True)),
                bool(payload.get("auto_uv", True)),
                export_mode,
            )
            task["result"] = {"ok": True, "output_paths": output_paths}
        except Exception as exc:
            task["result"] = {"ok": False, "error": str(exc)}
        finally:
            event.set()

    return 0.1


class _BridgeHandler(BaseHTTPRequestHandler):
    def _send(self, status_code, message):
        body = message.encode("utf-8", errors="replace")
        self.send_response(status_code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return

    def do_POST(self):
        if self.path != "/export_active":
            self._send(404, "ERROR|unknown_route")
            return

        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length > 0 else b""
        try:
            payload = json.loads(raw.decode("utf-8"))
            if "output_path" not in payload:
                raise ValueError("output_path is required")
        except Exception as exc:
            self._send(400, "ERROR|" + str(exc))
            return

        event = threading.Event()
        task = {"payload": payload, "event": event, "result": None}
        with _queue_lock:
            _task_queue.append(task)

        if not event.wait(timeout=180):
            self._send(504, "ERROR|timeout")
            return

        result = task.get("result") or {}
        if result.get("ok"):
            self._send(200, "OK|" + "|".join(result["output_paths"]))
        else:
            self._send(500, "ERROR|" + result.get("error", "unknown_error"))

    def do_GET(self):
        if self.path == "/health":
            self._send(200, "OK|alive")
            return
        self._send(404, "ERROR|unknown_route")


def _start_server():
    global _server, _server_thread, _last_server_error
    if _server is not None:
        _last_server_error = ""
        return

    try:
        _server = ThreadingHTTPServer(("127.0.0.1", 8765), _BridgeHandler)
    except OSError as exc:
        _last_server_error = f"Failed to bind 127.0.0.1:8765 ({exc})"
        raise RuntimeError(_last_server_error) from exc

    _server_thread = threading.Thread(target=_server.serve_forever, daemon=True)
    _server_thread.start()
    _last_server_error = ""

    if not bpy.app.timers.is_registered(_process_queue):
        bpy.app.timers.register(_process_queue, first_interval=0.1)


def _stop_server():
    global _server, _server_thread, _last_server_error
    if _server is not None:
        _server.shutdown()
        _server.server_close()
        _server = None
    _server_thread = None
    _last_server_error = ""

    if bpy.app.timers.is_registered(_process_queue):
        bpy.app.timers.unregister(_process_queue)


class BLENDERTOUE_OT_start_bridge_server(bpy.types.Operator):
    bl_idname = "blender_to_ue.start_bridge_server"
    bl_label = "Start Bridge Server"

    def execute(self, context):
        try:
            _start_server()
            self.report({"INFO"}, "Blender To UE bridge server started on 127.0.0.1:8765")
            return {"FINISHED"}
        except RuntimeError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}


class BLENDERTOUE_OT_stop_bridge_server(bpy.types.Operator):
    bl_idname = "blender_to_ue.stop_bridge_server"
    bl_label = "Stop Bridge Server"

    def execute(self, context):
        _stop_server()
        self.report({"INFO"}, "Blender To UE bridge server stopped")
        return {"FINISHED"}


class BLENDERTOUE_PT_live_bridge_panel(bpy.types.Panel):
    bl_label = "Blender To UE Live Bridge"
    bl_idname = "BLENDERTOUE_PT_live_bridge_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "BlenderToUE"

    def draw(self, context):
        layout = self.layout
        status = "Connected" if _server is not None else "Disconnected"
        layout.label(text=f"UE endpoint: 127.0.0.1:8765 ({status})")
        if _last_server_error:
            layout.label(text=_last_server_error, icon="ERROR")
        layout.operator("blender_to_ue.start_bridge_server")
        layout.operator("blender_to_ue.stop_bridge_server")


_CLASSES = (
    BLENDERTOUE_OT_start_bridge_server,
    BLENDERTOUE_OT_stop_bridge_server,
    BLENDERTOUE_PT_live_bridge_panel,
)


def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    try:
        _start_server()
    except RuntimeError:
        pass


def unregister():
    _stop_server()
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
