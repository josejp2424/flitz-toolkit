#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate Flitz gettext PO and MO files without requiring msgfmt."""
from __future__ import annotations
import ast
import re
import struct
from pathlib import Path

from translation_supplement import TRANSLATIONS as SUPPLEMENT_TRANSLATIONS

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
PO = ROOT / "po"
LOCALE = ROOT / "locale"
PATTERN = re.compile(r'_\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)', re.S)


def messages() -> list[str]:
    result: list[str] = []
    for path in sorted(SRC.glob("*.c")):
        text = path.read_text(encoding="utf-8")
        for match in PATTERN.finditer(text):
            parts = re.findall(r'"(?:\\.|[^"\\])*"', match.group(1))
            value = "".join(ast.literal_eval(part) for part in parts)
            if value not in result:
                result.append(value)
    return result


def q(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def write_po(language: str, mapping: dict[str, str], msgids: list[str]) -> Path:
    PO.mkdir(exist_ok=True)
    path = PO / f"{language}.po"
    lines = [
        'msgid ""',
        'msgstr ""',
        q("Project-Id-Version: flitz-toolkit 6.0\n"),
        q(f"Language: {language}\n"),
        q("MIME-Version: 1.0\n"),
        q("Content-Type: text/plain; charset=UTF-8\n"),
        q("Content-Transfer-Encoding: 8bit\n"),
        "",
    ]
    for msgid in msgids:
        lines += [f"msgid {q(msgid)}", f"msgstr {q(mapping.get(msgid, ''))}", ""]
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


def write_pot(msgids: list[str]) -> None:
    PO.mkdir(exist_ok=True)
    lines = [
        'msgid ""', 'msgstr ""',
        q("Project-Id-Version: flitz-toolkit 6.0\n"),
        q("MIME-Version: 1.0\n"),
        q("Content-Type: text/plain; charset=UTF-8\n"),
        "",
    ]
    for msgid in msgids:
        lines += [f"msgid {q(msgid)}", 'msgstr ""', ""]
    (PO / "flitz-toolkit.pot").write_text("\n".join(lines), encoding="utf-8")


def write_mo(language: str, mapping: dict[str, str], msgids: list[str]) -> None:
    header = (
        "Project-Id-Version: flitz-toolkit 6.0\n"
        f"Language: {language}\n"
        "MIME-Version: 1.0\n"
        "Content-Type: text/plain; charset=UTF-8\n"
        "Content-Transfer-Encoding: 8bit\n"
    )
    catalog = {"": header}
    catalog.update({m: mapping[m] for m in msgids if mapping.get(m)})
    keys = sorted(catalog)
    ids = b""
    strs = b""
    offsets_ids: list[tuple[int, int]] = []
    offsets_strs: list[tuple[int, int]] = []
    for key in keys:
        data = key.encode("utf-8")
        offsets_ids.append((len(data), len(ids)))
        ids += data + b"\0"
    for key in keys:
        data = catalog[key].encode("utf-8")
        offsets_strs.append((len(data), len(strs)))
        strs += data + b"\0"
    n = len(keys)
    keystart = 7 * 4
    valuestart = keystart + n * 8
    idstart = valuestart + n * 8
    strstart = idstart + len(ids)
    out = [struct.pack("<7I", 0x950412DE, 0, n, keystart, valuestart, 0, 0)]
    out.extend(struct.pack("<2I", length, idstart + offset) for length, offset in offsets_ids)
    out.extend(struct.pack("<2I", length, strstart + offset) for length, offset in offsets_strs)
    out += [ids, strs]
    target = LOCALE / language / "LC_MESSAGES"
    target.mkdir(parents=True, exist_ok=True)
    (target / "flitz-toolkit.mo").write_bytes(b"".join(out))


ES = {
'Skipped existing: %s':'Omitido porque ya existe: %s','Extracted: %s':'Extraído: %s','Skipped unsupported special file: %s':'Omitido archivo especial no compatible: %s','%d%% completed':'%d%% completado','Extracting...':'Extrayendo...','Running: %s':'Ejecutando: %s','Required tool is not installed: %s':'La herramienta necesaria no está instalada: %s','Neither unzip nor 7z is installed.':'No están instalados ni unzip ni 7z.','The DEB package has no data archive.':'El paquete DEB no contiene un archivo de datos.','Install dpkg-deb, or install ar and tar.':'Instale dpkg-deb o instale ar y tar.','innoextract could not extract it; trying 7z.':'innoextract no pudo extraerlo; se probará con 7z.','Cannot inspect %s: %s':'No se puede examinar %s: %s','Cannot make AppImage executable: %s':'No se puede hacer ejecutable el AppImage: %s','Cannot move decompressed file: %s':'No se puede mover el archivo descomprimido: %s','Unknown extension; trying the universal 7z handler.':'Extensión desconocida; se probará el manejador universal de 7z.','Recursively extracting: %s':'Extrayendo recursivamente: %s','Writing files...':'Escribiendo archivos...','Extraction completed':'Extracción completada','Extraction completed successfully!':'¡Extracción completada correctamente!','Success':'Éxito','Extraction failed':'Falló la extracción','Error: %s':'Error: %s','Detected format: %s':'Formato detectado: %s','Searching for archives inside the extracted data...':'Buscando archivos comprimidos dentro de los datos extraídos...','Files were extracted to:\n%s':'Los archivos se extrajeron en:\n%s','Unknown extraction error.':'Error de extracción desconocido.','Warning: temporary directory could not be removed: %s':'Advertencia: no se pudo eliminar el directorio temporal: %s','Error':'Error','Please select a valid archive first.':'Seleccione primero un archivo comprimido válido.','Starting extraction...':'Iniciando extracción...','File: %s':'Archivo: %s','Output: %s':'Salida: %s','Select archive':'Seleccionar archivo comprimido','Cancel':'Cancelar','Select':'Seleccionar','All supported archives':'Todos los archivos compatibles','Select output directory':'Seleccionar directorio de salida','Operation in progress':'Operación en curso','Wait for extraction to finish before closing Flitz.':'Espere a que termine la extracción antes de cerrar Flitz.','Flitz Universal Extractor':'Extractor universal Flitz','<b>Flitz Universal Extractor</b>':'<b>Extractor universal Flitz</b>','by josejp2424 (v6.0 C)':'por josejp2424 (v6.0 C)','File':'Archivo','Drag file here or click Select':'Arrastre el archivo aquí o pulse Seleccionar','Output':'Salida','Same folder as the archive':'La misma carpeta del archivo','Options':'Opciones','Recursive':'Recursiva','Keep structure':'Mantener estructura','Overwrite':'Sobrescribir','Ready':'Listo','Console':'Consola','Extract':'Extraer','The destination already exists:\n%s\n\nReplace it?':'El destino ya existe:\n%s\n\n¿Desea reemplazarlo?','Replace existing file?':'¿Reemplazar el archivo existente?','Compressing...':'Comprimiendo...','Required tool is not installed: tar':'La herramienta necesaria no está instalada: tar','Required tool is not installed: zip':'La herramienta necesaria no está instalada: zip','Required tool is not installed: 7z':'La herramienta necesaria no está instalada: 7z','mksquashfs is not installed.':'mksquashfs no está instalado.','Format not supported':'Formato no compatible','Completed successfully':'Completado correctamente','Completed successfully.':'Completado correctamente.','Compression failed':'Falló la compresión','File created:\n%s':'Archivo creado:\n%s','Unknown compression error.':'Error de compresión desconocido.','Please select a valid folder first.':'Seleccione primero una carpeta válida.','Enter an output filename.':'Escriba un nombre para el archivo de salida.','The output filename must not contain a slash.':'El nombre del archivo de salida no debe contener una barra.','Starting compression...':'Iniciando compresión...','Source: %s':'Origen: %s','Select folder to compress':'Seleccionar carpeta para comprimir','Warning':'Advertencia','Please drag only folders.':'Arrastre solamente carpetas.','Wait for compression to finish before closing Flitz.':'Espere a que termine la compresión antes de cerrar Flitz.','Flitz Compressor':'Compresor Flitz','<b>Flitz Compressor</b>':'<b>Compresor Flitz</b>','Source':'Origen','Drag folder here or click Select':'Arrastre la carpeta aquí o pulse Seleccionar','Format':'Formato','SquashFS Options':'Opciones de SquashFS','Compression:':'Compresión:','Block size:':'Tamaño de bloque:','Advanced XZ Options':'Opciones avanzadas de XZ','BCJ Filter:':'Filtro BCJ:','Output Directory':'Directorio de salida','Default: source folder':'Predeterminado: carpeta de origen','Output Filename':'Nombre del archivo de salida','Compress':'Comprimir','Warning: missing optional tools: %s':'Advertencia: faltan herramientas opcionales: %s','Choose an output directory outside the source folder.':'Elija un directorio de salida fuera de la carpeta de origen.','Current':'Actual'
}

ES.update({
    'Created by josejp2424 for Essora Linux.': 'Creado por josejp2424 para Essora Linux.',
    'Copyright © 2026 josejp2424': 'Copyright © 2026 josejp2424',
    'About': 'Acerca de',
})

# Common visible-interface translations. Unlisted technical details fall back to English.
COMMON = {
'ca': {'Flitz Universal Extractor':'Extractor universal Flitz','<b>Flitz Universal Extractor</b>':'<b>Extractor universal Flitz</b>','Flitz Compressor':'Compressor Flitz','<b>Flitz Compressor</b>':'<b>Compressor Flitz</b>','File':'Fitxer','Source':'Origen','Output':'Sortida','Select':'Selecciona','Cancel':'Cancel·la','Options':'Opcions','Recursive':'Recursiu','Keep structure':'Mantén l’estructura','Overwrite':'Sobreescriu','Ready':'Preparat','Console':'Consola','Extract':'Extreu','Compress':'Comprimeix','Format':'Format','Output Directory':'Directori de sortida','Output Filename':'Nom del fitxer de sortida','Compression:':'Compressió:','Block size:':'Mida del bloc:','Success':'Èxit','Error':'Error','Warning':'Avís','Extraction completed':'Extracció completada','Extraction failed':'Ha fallat l’extracció','Compression failed':'Ha fallat la compressió','Completed successfully':'Completat correctament','Starting extraction...':'Iniciant l’extracció...','Starting compression...':'Iniciant la compressió...'},
'de': {'Flitz Universal Extractor':'Flitz Universal-Entpacker','<b>Flitz Universal Extractor</b>':'<b>Flitz Universal-Entpacker</b>','Flitz Compressor':'Flitz-Kompressor','<b>Flitz Compressor</b>':'<b>Flitz-Kompressor</b>','File':'Datei','Source':'Quelle','Output':'Ausgabe','Select':'Auswählen','Cancel':'Abbrechen','Options':'Optionen','Recursive':'Rekursiv','Keep structure':'Struktur beibehalten','Overwrite':'Überschreiben','Ready':'Bereit','Console':'Konsole','Extract':'Entpacken','Compress':'Komprimieren','Format':'Format','Output Directory':'Ausgabeverzeichnis','Output Filename':'Ausgabedateiname','Compression:':'Kompression:','Block size:':'Blockgröße:','Success':'Erfolg','Error':'Fehler','Warning':'Warnung','Extraction completed':'Entpacken abgeschlossen','Extraction failed':'Entpacken fehlgeschlagen','Compression failed':'Komprimierung fehlgeschlagen','Completed successfully':'Erfolgreich abgeschlossen','Starting extraction...':'Entpacken wird gestartet...','Starting compression...':'Komprimierung wird gestartet...'},
'fr': {'Flitz Universal Extractor':'Extracteur universel Flitz','<b>Flitz Universal Extractor</b>':'<b>Extracteur universel Flitz</b>','Flitz Compressor':'Compresseur Flitz','<b>Flitz Compressor</b>':'<b>Compresseur Flitz</b>','File':'Fichier','Source':'Source','Output':'Sortie','Select':'Sélectionner','Cancel':'Annuler','Options':'Options','Recursive':'Récursif','Keep structure':'Conserver la structure','Overwrite':'Écraser','Ready':'Prêt','Console':'Console','Extract':'Extraire','Compress':'Compresser','Format':'Format','Output Directory':'Dossier de sortie','Output Filename':'Nom du fichier de sortie','Compression:':'Compression :','Block size:':'Taille de bloc :','Success':'Succès','Error':'Erreur','Warning':'Avertissement','Extraction completed':'Extraction terminée','Extraction failed':'Échec de l’extraction','Compression failed':'Échec de la compression','Completed successfully':'Terminé avec succès','Starting extraction...':'Démarrage de l’extraction...','Starting compression...':'Démarrage de la compression...'},
'it': {'Flitz Universal Extractor':'Estrattore universale Flitz','<b>Flitz Universal Extractor</b>':'<b>Estrattore universale Flitz</b>','Flitz Compressor':'Compressore Flitz','<b>Flitz Compressor</b>':'<b>Compressore Flitz</b>','File':'File','Source':'Origine','Output':'Destinazione','Select':'Seleziona','Cancel':'Annulla','Options':'Opzioni','Recursive':'Ricorsivo','Keep structure':'Mantieni struttura','Overwrite':'Sovrascrivi','Ready':'Pronto','Console':'Console','Extract':'Estrai','Compress':'Comprimi','Format':'Formato','Output Directory':'Directory di destinazione','Output Filename':'Nome file di destinazione','Compression:':'Compressione:','Block size:':'Dimensione blocco:','Success':'Operazione riuscita','Error':'Errore','Warning':'Avviso','Extraction completed':'Estrazione completata','Extraction failed':'Estrazione non riuscita','Compression failed':'Compressione non riuscita','Completed successfully':'Completato correttamente','Starting extraction...':'Avvio estrazione...','Starting compression...':'Avvio compressione...'},
'pt': {'Flitz Universal Extractor':'Extrator universal Flitz','<b>Flitz Universal Extractor</b>':'<b>Extrator universal Flitz</b>','Flitz Compressor':'Compressor Flitz','<b>Flitz Compressor</b>':'<b>Compressor Flitz</b>','File':'Arquivo','Source':'Origem','Output':'Saída','Select':'Selecionar','Cancel':'Cancelar','Options':'Opções','Recursive':'Recursivo','Keep structure':'Manter estrutura','Overwrite':'Sobrescrever','Ready':'Pronto','Console':'Console','Extract':'Extrair','Compress':'Compactar','Format':'Formato','Output Directory':'Diretório de saída','Output Filename':'Nome do arquivo de saída','Compression:':'Compressão:','Block size:':'Tamanho do bloco:','Success':'Sucesso','Error':'Erro','Warning':'Aviso','Extraction completed':'Extração concluída','Extraction failed':'Falha na extração','Compression failed':'Falha na compactação','Completed successfully':'Concluído com sucesso','Starting extraction...':'Iniciando extração...','Starting compression...':'Iniciando compactação...'},
'hu': {'Flitz Universal Extractor':'Flitz univerzális kicsomagoló','<b>Flitz Universal Extractor</b>':'<b>Flitz univerzális kicsomagoló</b>','Flitz Compressor':'Flitz tömörítő','<b>Flitz Compressor</b>':'<b>Flitz tömörítő</b>','File':'Fájl','Source':'Forrás','Output':'Kimenet','Select':'Kiválasztás','Cancel':'Mégse','Options':'Beállítások','Recursive':'Rekurzív','Keep structure':'Könyvtárszerkezet megtartása','Overwrite':'Felülírás','Ready':'Kész','Console':'Konzol','Extract':'Kicsomagolás','Compress':'Tömörítés','Format':'Formátum','Output Directory':'Kimeneti könyvtár','Output Filename':'Kimeneti fájlnév','Compression:':'Tömörítés:','Block size:':'Blokkméret:','Success':'Siker','Error':'Hiba','Warning':'Figyelmeztetés','Extraction completed':'Kicsomagolás befejezve','Extraction failed':'A kicsomagolás sikertelen','Compression failed':'A tömörítés sikertelen','Completed successfully':'Sikeresen befejezve'},
'ru': {'Flitz Universal Extractor':'Универсальный распаковщик Flitz','<b>Flitz Universal Extractor</b>':'<b>Универсальный распаковщик Flitz</b>','Flitz Compressor':'Архиватор Flitz','<b>Flitz Compressor</b>':'<b>Архиватор Flitz</b>','File':'Файл','Source':'Источник','Output':'Вывод','Select':'Выбрать','Cancel':'Отмена','Options':'Параметры','Recursive':'Рекурсивно','Keep structure':'Сохранять структуру','Overwrite':'Перезаписывать','Ready':'Готово','Console':'Консоль','Extract':'Извлечь','Compress':'Сжать','Format':'Формат','Output Directory':'Каталог вывода','Output Filename':'Имя выходного файла','Compression:':'Сжатие:','Block size:':'Размер блока:','Success':'Успешно','Error':'Ошибка','Warning':'Предупреждение','Extraction completed':'Извлечение завершено','Extraction failed':'Ошибка извлечения','Compression failed':'Ошибка сжатия','Completed successfully':'Успешно завершено'},
'ja': {'Flitz Universal Extractor':'Flitz 汎用展開ツール','<b>Flitz Universal Extractor</b>':'<b>Flitz 汎用展開ツール</b>','Flitz Compressor':'Flitz 圧縮ツール','<b>Flitz Compressor</b>':'<b>Flitz 圧縮ツール</b>','File':'ファイル','Source':'元フォルダー','Output':'出力','Select':'選択','Cancel':'キャンセル','Options':'オプション','Recursive':'再帰的','Keep structure':'構造を維持','Overwrite':'上書き','Ready':'準備完了','Console':'コンソール','Extract':'展開','Compress':'圧縮','Format':'形式','Output Directory':'出力先フォルダー','Output Filename':'出力ファイル名','Compression:':'圧縮方式:','Block size:':'ブロックサイズ:','Success':'成功','Error':'エラー','Warning':'警告','Extraction completed':'展開が完了しました','Extraction failed':'展開に失敗しました','Compression failed':'圧縮に失敗しました','Completed successfully':'正常に完了しました'},
'zh': {'Flitz Universal Extractor':'Flitz 通用解压工具','<b>Flitz Universal Extractor</b>':'<b>Flitz 通用解压工具</b>','Flitz Compressor':'Flitz 压缩工具','<b>Flitz Compressor</b>':'<b>Flitz 压缩工具</b>','File':'文件','Source':'源文件夹','Output':'输出','Select':'选择','Cancel':'取消','Options':'选项','Recursive':'递归','Keep structure':'保留目录结构','Overwrite':'覆盖','Ready':'就绪','Console':'控制台','Extract':'解压','Compress':'压缩','Format':'格式','Output Directory':'输出目录','Output Filename':'输出文件名','Compression:':'压缩方式：','Block size:':'块大小：','Success':'成功','Error':'错误','Warning':'警告','Extraction completed':'解压完成','Extraction failed':'解压失败','Compression failed':'压缩失败','Completed successfully':'已成功完成'},
'ar': {'Flitz Universal Extractor':'مستخرج Flitz الشامل','<b>Flitz Universal Extractor</b>':'<b>مستخرج Flitz الشامل</b>','Flitz Compressor':'ضاغط Flitz','<b>Flitz Compressor</b>':'<b>ضاغط Flitz</b>','File':'الملف','Source':'المصدر','Output':'الإخراج','Select':'اختيار','Cancel':'إلغاء','Options':'الخيارات','Recursive':'استخراج متداخل','Keep structure':'الاحتفاظ بالبنية','Overwrite':'الكتابة فوق الموجود','Ready':'جاهز','Console':'وحدة التحكم','Extract':'استخراج','Compress':'ضغط','Format':'الصيغة','Output Directory':'مجلد الإخراج','Output Filename':'اسم ملف الإخراج','Compression:':'الضغط:','Block size:':'حجم الكتلة:','Success':'نجاح','Error':'خطأ','Warning':'تحذير','Extraction completed':'اكتمل الاستخراج','Extraction failed':'فشل الاستخراج','Compression failed':'فشل الضغط','Completed successfully':'اكتملت العملية بنجاح'}
}

PET_TRANSLATIONS = {
'es': {
'Cannot read PET package: %s':'No se puede leer el paquete PET: %s',
'PET package ended unexpectedly.':'El paquete PET terminó de forma inesperada.',
'Cannot write temporary PET archive: %s':'No se puede escribir el archivo PET temporal: %s',
'Legacy PET without checksum footer; extracting directly.':'PET antiguo sin suma de comprobación final; se extraerá directamente.',
'Cannot create temporary PET archive: %s':'No se puede crear el archivo PET temporal: %s',
'PET checksum does not match. The package may be damaged.':'La suma de comprobación del PET no coincide. El paquete puede estar dañado.',
'PET checksum verified.':'Suma de comprobación PET verificada.'},
'ca': {
'Cannot read PET package: %s':'No es pot llegir el paquet PET: %s',
'PET package ended unexpectedly.':'El paquet PET ha finalitzat inesperadament.',
'Cannot write temporary PET archive: %s':'No es pot escriure l’arxiu PET temporal: %s',
'Legacy PET without checksum footer; extracting directly.':'PET antic sense suma de verificació final; s’extraurà directament.',
'Cannot create temporary PET archive: %s':'No es pot crear l’arxiu PET temporal: %s',
'PET checksum does not match. The package may be damaged.':'La suma de verificació del PET no coincideix. El paquet pot estar malmès.',
'PET checksum verified.':'Suma de verificació PET comprovada.'},
'de': {
'Cannot read PET package: %s':'PET-Paket kann nicht gelesen werden: %s',
'PET package ended unexpectedly.':'Das PET-Paket endete unerwartet.',
'Cannot write temporary PET archive: %s':'Temporäres PET-Archiv kann nicht geschrieben werden: %s',
'Legacy PET without checksum footer; extracting directly.':'Älteres PET ohne Prüfsummen-Fußzeile; wird direkt entpackt.',
'Cannot create temporary PET archive: %s':'Temporäres PET-Archiv kann nicht erstellt werden: %s',
'PET checksum does not match. The package may be damaged.':'Die PET-Prüfsumme stimmt nicht überein. Das Paket könnte beschädigt sein.',
'PET checksum verified.':'PET-Prüfsumme bestätigt.'},
'fr': {
'Cannot read PET package: %s':'Impossible de lire le paquet PET : %s',
'PET package ended unexpectedly.':'Le paquet PET s’est terminé de façon inattendue.',
'Cannot write temporary PET archive: %s':'Impossible d’écrire l’archive PET temporaire : %s',
'Legacy PET without checksum footer; extracting directly.':'Ancien PET sans somme de contrôle finale ; extraction directe.',
'Cannot create temporary PET archive: %s':'Impossible de créer l’archive PET temporaire : %s',
'PET checksum does not match. The package may be damaged.':'La somme de contrôle PET ne correspond pas. Le paquet est peut-être endommagé.',
'PET checksum verified.':'Somme de contrôle PET vérifiée.'},
'it': {
'Cannot read PET package: %s':'Impossibile leggere il pacchetto PET: %s',
'PET package ended unexpectedly.':'Il pacchetto PET è terminato inaspettatamente.',
'Cannot write temporary PET archive: %s':'Impossibile scrivere l’archivio PET temporaneo: %s',
'Legacy PET without checksum footer; extracting directly.':'PET precedente senza checksum finale; estrazione diretta.',
'Cannot create temporary PET archive: %s':'Impossibile creare l’archivio PET temporaneo: %s',
'PET checksum does not match. The package may be damaged.':'Il checksum PET non corrisponde. Il pacchetto potrebbe essere danneggiato.',
'PET checksum verified.':'Checksum PET verificato.'},
'pt': {
'Cannot read PET package: %s':'Não foi possível ler o pacote PET: %s',
'PET package ended unexpectedly.':'O pacote PET terminou inesperadamente.',
'Cannot write temporary PET archive: %s':'Não foi possível gravar o arquivo PET temporário: %s',
'Legacy PET without checksum footer; extracting directly.':'PET antigo sem soma de verificação final; extraindo diretamente.',
'Cannot create temporary PET archive: %s':'Não foi possível criar o arquivo PET temporário: %s',
'PET checksum does not match. The package may be damaged.':'A soma de verificação do PET não confere. O pacote pode estar danificado.',
'PET checksum verified.':'Soma de verificação PET confirmada.'},
'hu': {
'Cannot read PET package: %s':'A PET csomag nem olvasható: %s',
'PET package ended unexpectedly.':'A PET csomag váratlanul véget ért.',
'Cannot write temporary PET archive: %s':'Az ideiglenes PET archívum nem írható: %s',
'Legacy PET without checksum footer; extracting directly.':'Régi PET ellenőrzőösszeg nélkül; közvetlen kicsomagolás.',
'Cannot create temporary PET archive: %s':'Az ideiglenes PET archívum nem hozható létre: %s',
'PET checksum does not match. The package may be damaged.':'A PET ellenőrzőösszege nem egyezik. A csomag sérült lehet.',
'PET checksum verified.':'A PET ellenőrzőösszeg rendben.'},
'ru': {
'Cannot read PET package: %s':'Не удалось прочитать пакет PET: %s',
'PET package ended unexpectedly.':'Пакет PET неожиданно закончился.',
'Cannot write temporary PET archive: %s':'Не удалось записать временный архив PET: %s',
'Legacy PET without checksum footer; extracting directly.':'Старый PET без контрольной суммы в конце; выполняется прямое извлечение.',
'Cannot create temporary PET archive: %s':'Не удалось создать временный архив PET: %s',
'PET checksum does not match. The package may be damaged.':'Контрольная сумма PET не совпадает. Пакет может быть повреждён.',
'PET checksum verified.':'Контрольная сумма PET проверена.'},
'ja': {
'Cannot read PET package: %s':'PET パッケージを読み込めません: %s',
'PET package ended unexpectedly.':'PET パッケージが予期せず終了しました。',
'Cannot write temporary PET archive: %s':'一時 PET アーカイブを書き込めません: %s',
'Legacy PET without checksum footer; extracting directly.':'チェックサム末尾のない旧形式 PET を直接展開します。',
'Cannot create temporary PET archive: %s':'一時 PET アーカイブを作成できません: %s',
'PET checksum does not match. The package may be damaged.':'PET チェックサムが一致しません。パッケージが破損している可能性があります。',
'PET checksum verified.':'PET チェックサムを確認しました。'},
'zh': {
'Cannot read PET package: %s':'无法读取 PET 软件包：%s',
'PET package ended unexpectedly.':'PET 软件包意外结束。',
'Cannot write temporary PET archive: %s':'无法写入临时 PET 归档：%s',
'Legacy PET without checksum footer; extracting directly.':'旧版 PET 没有末尾校验和；将直接解压。',
'Cannot create temporary PET archive: %s':'无法创建临时 PET 归档：%s',
'PET checksum does not match. The package may be damaged.':'PET 校验和不匹配，软件包可能已损坏。',
'PET checksum verified.':'PET 校验和验证成功。'},
'ar': {
'Cannot read PET package: %s':'تعذرت قراءة حزمة PET: %s',
'PET package ended unexpectedly.':'انتهت حزمة PET بشكل غير متوقع.',
'Cannot write temporary PET archive: %s':'تعذرت كتابة أرشيف PET المؤقت: %s',
'Legacy PET without checksum footer; extracting directly.':'حزمة PET قديمة بلا تذييل تحقق؛ سيتم الاستخراج مباشرة.',
'Cannot create temporary PET archive: %s':'تعذر إنشاء أرشيف PET المؤقت: %s',
'PET checksum does not match. The package may be damaged.':'مجموع تحقق PET غير مطابق. قد تكون الحزمة تالفة.',
'PET checksum verified.':'تم التحقق من مجموع PET.'}
}

ES.update(PET_TRANSLATIONS['es'])
for _language, _mapping in COMMON.items():
    _mapping.update(PET_TRANSLATIONS.get(_language, {}))
    _mapping.update(SUPPLEMENT_TRANSLATIONS.get(_language, {}))


def validate_catalog(language: str, mapping: dict[str, str], msgids: list[str]) -> None:
    missing = [msgid for msgid in msgids if not mapping.get(msgid)]
    if missing:
        raise RuntimeError(
            f"{language}: {len(missing)} untranslated messages: "
            + ", ".join(repr(item) for item in missing[:5])
        )


def main() -> None:
    obsolete_pot = PO / "flitz.pot"
    if obsolete_pot.exists():
        obsolete_pot.unlink()
    for obsolete_mo in LOCALE.glob("*/LC_MESSAGES/flitz.mo"):
        obsolete_mo.unlink()

    msgids = messages()
    write_pot(msgids)
    translations = {'es': ES, **COMMON}
    for language, mapping in translations.items():
        validate_catalog(language, mapping, msgids)
        write_po(language, mapping, msgids)
        write_mo(language, mapping, msgids)
    print(f"Generated {len(msgids)} messages for {len(translations)} languages")

if __name__ == '__main__':
    main()
