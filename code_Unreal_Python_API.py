import unreal
import os
import tkinter as tk
from tkinter import filedialog

# Открытие проводника для выбора файла
def select_file():
    root = tk.Tk()
    root.withdraw()  # Скрываем главное окно Tkinter
    file_path = filedialog.askopenfilename(
        title="Выберите модель",
        filetypes=[("3D модели", "*.fbx *.glb")]  # Фильтр по типу файлов
    )
    return file_path

# Функция для создания папки в Content Browser
def create_folder_in_unreal(folder_path):
    if not unreal.EditorAssetLibrary.does_directory_exist(folder_path):
        unreal.EditorAssetLibrary.make_directory(folder_path)
        print(f"Создана папка: {folder_path}")

# Импорт в Unreal с установкой Complex Collision
def import_to_unreal(model_name, export_path):
    # Создаём папку для этой модели
    model_folder = f"/Game/ImportedModels/{model_name}"
    create_folder_in_unreal(model_folder)  

    asset_name = model_name  
    asset_path = f"{model_folder}/{asset_name}"  # Полный путь к ассету

    task = unreal.AssetImportTask()
    task.filename = export_path
    task.destination_path = model_folder  # Используем созданную папку
    task.destination_name = asset_name
    task.replace_existing = True
    task.automated = True
    task.save = True

    # Запускаем импорт
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    print(f"Модель импортирована в Unreal Engine в папку: {model_folder}")

# Запуск выбора файла
selected_file = select_file()

if selected_file:
    model_name = os.path.splitext(os.path.basename(selected_file))[0]  # Извлекаем имя файла без расширения
    import_to_unreal(model_name, selected_file)
else:
    print("Файл не был выбран.")