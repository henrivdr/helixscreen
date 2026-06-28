# Translation System

HelixScreen's translation system is built on LVGL's native XML translations. Source strings live in per-locale YAML files; a generator turns them into runtime XML translation packs that the app loads at startup, and C++ resolves strings at runtime through LVGL's `lv_translation_*` API (`lv_tr()`). This document explains how translations flow from source YAML files through generation to the running application.

## Supported Languages

| Code | Language   |
|------|------------|
| de   | German     |
| en   | English    |
| es   | Spanish    |
| fr   | French     |
| it   | Italian    |
| ja   | Japanese   |
| pt   | Portuguese |
| ru   | Russian    |
| zh   | Chinese    |

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           translations/*.yml                            │
│                    (Master source files - one per locale)               │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                                 ▼
                   ┌─────────────────────────────┐
                   │  generate_translations.py   │
                   │  (Python pack generator)    │
                   └─────────────┬───────────────┘
                                 │
                ┌────────────────┴────────────────┐
                ▼                                  ▼
┌───────────────────────────┐      ┌──────────────────────────────┐
│  translations.xml         │      │  <lang>.xml (per-locale)     │
│  (combined LVGL XML pack) │      │  en.xml, de.xml, zh.xml, ... │
└─────────────┬─────────────┘      └───────────────┬──────────────┘
              │                                     │
              ▼                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          Runtime (Application)                          │
│  • App loads the active locale's pack via the locale loader             │
│    (lv_translation_set_language + lv_xml_register_translation_from_file) │
│  • XML parser resolves translation_tag / placeholder_tag via lv_tr()    │
│  • C++ resolves strings with lv_tr("English text") (English fallback)   │
│  • Plural rules applied per-language                                    │
└─────────────────────────────────────────────────────────────────────────┘
```

> The generator emits **only** the runtime XML packs by default. The legacy
> `lv_i18n` C table (`src/generated/lv_i18n_translations.c`/`.h`) was never
> compiled or linked — the runtime uses LVGL's native `lv_translation_*` API on
> the XML — so it has been removed from the repo. It can still be produced on
> demand with `generate_translations.py --emit-lv-i18n`, but normal work never
> needs it.

## Source Files (YAML)

Translation source files live in `translations/` with one file per locale.

### File Format

```yaml
locale: en
translations:
  # Simple singular strings
  "Cancel": "Cancel"
  "Save": "Save"
  "Temperature": "Temperature"

  # Strings with special characters need quoting
  "Nozzle: {temp}°C": "Nozzle: {temp}°C"

  # Plural forms (dictionary)
  "items_selected":
    one: "{count} item selected"
    other: "{count} items selected"
```

### Key Rules

- **Keys are English text** - The translation key is the English version of the string
- **Singular translations** - Direct string-to-string mapping
- **Plural translations** - Dictionary with CLDR plural categories

### Example: Russian with Complex Plurals

Russian uses one/few/many/other categories:

```yaml
locale: ru
translations:
  "items_selected":
    one: "{count} элемент выбран"       # 1, 21, 31...
    few: "{count} элемента выбрано"     # 2-4, 22-24...
    many: "{count} элементов выбрано"   # 5-20, 25-30...
    other: "{count} элемента выбрано"   # Decimals
```

## Build Pipeline

### Generator Script

`scripts/generate_translations.py` is the main pack generator. It reads all YAML files and produces the runtime XML translation packs:

| Output | Path | Purpose |
|--------|------|---------|
| `translations.xml` | `ui_xml/translations/translations.xml` | Combined LVGL XML pack (all languages) |
| `<lang>.xml` | `ui_xml/translations/<lang>.xml` | Per-locale packs (`en.xml`, `de.xml`, `zh.xml`, …) loaded individually at runtime |

These are the only outputs by default. The legacy `lv_i18n` C/H table is **not**
emitted (see the note in the Architecture section); pass `--emit-lv-i18n` to
produce it on demand.

### Makefile Integration

Translation generation is integrated into the build via `mk/translations.mk`:

```makefile
# Regenerate the XML packs if YAML or script changes
$(TRANS_XML): $(TRANS_YAML) $(TRANS_SCRIPT)
    python generate_translations.py
```

Run manually with:

```bash
make translations
```

## Generated Files

### translations.xml

LVGL's native XML translation format. Each `<translation>` element maps a tag to translations in each language:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<translations languages="de en es fr it ja pt ru zh">
  <translation tag="Cancel" de="Abbrechen" en="Cancel" es="Cancelar" fr="Annuler" .../>
  <translation tag="Save" de="Speichern" en="Save" es="Guardar" fr="Enregistrer" .../>
  <translation tag="Temperature" de="Temperatur" en="Temperature" es="Temperatura" .../>
</translations>
```

### Per-locale packs (`<lang>.xml`)

Alongside the combined `translations.xml`, the generator emits one pack per
locale (`en.xml`, `de.xml`, `zh.xml`, …). At runtime the app loads only the
active locale's pack via the locale loader (`src/system/translation_loader.cpp`,
`helix::ui::ensure_translation_loaded()` → `lv_xml_register_translation_from_file()`),
so it never has to parse every language up front.

### Legacy `lv_i18n` C table (opt-in)

`src/generated/lv_i18n_translations.c`/`.h` was a parallel i18n subsystem kept
alongside LVGL's native `lv_translation_*` API. It was never read from
(`lv_i18n_get_text()` had no callers), so it has been removed from the repo and
is no longer generated by default. Pass `generate_translations.py --emit-lv-i18n`
if you need to produce it for some external consumer — normal HelixScreen work
never does.

## XML Usage

### Static Text with translation_tag

For labels whose text is known at compile time, use the `translation_tag` attribute:

```xml
<!-- Text translates based on current locale -->
<text_heading text="Temperature" translation_tag="Temperature"/>

<!-- Button with translated label -->
<ui_button text="Cancel" translation_tag="Cancel" variant="secondary"/>

<!-- Help text -->
<text_small text="Select 'None' if you don't have a sensor."
            translation_tag="Select 'None' if you don't have a sensor."/>
```

The `text` attribute serves as a fallback if the translation tag isn't found.

### Semantic Text Widgets

Use semantic text widgets (`text_heading`, `text_body`, `text_small`, `text_muted`) rather than raw `lv_label`:

```xml
<!-- ✅ Correct -->
<text_body text="Material" translation_tag="Material"/>

<!-- ❌ Avoid -->
<lv_label text="Material" style_text_font="..." translation_tag="Material"/>
```

### Dynamic Text with bind_text

For text that changes at runtime based on data, use `bind_text` with subjects instead of `translation_tag`:

```xml
<!-- Subject-bound text (NOT translated) -->
<text_body bind_text="printer_status_subject"/>

<!-- Temperature display from subject -->
<text_heading bind_text="nozzle_temp_display"/>
```

**Important:** `bind_text` and `translation_tag` serve different purposes:
- `translation_tag` - Static text that varies by locale
- `bind_text` - Dynamic text that varies by application state

### Text-input Placeholders with placeholder_tag

Text inputs translate their placeholder text via `placeholder_tag`, which
mirrors `translation_tag` on labels and is resolved through `lv_tr()` at parse
time:

```xml
<text_input placeholder_text="Search vendors..." placeholder_tag="Search vendors..."/>
```

The `form_field` component forwards the same attribute, so wrap a placeholder
the same way there:

```xml
<form_field placeholder="Enter network name" placeholder_tag="Enter network name"/>
```

As with `translation_tag`, the literal `placeholder_text` / `placeholder`
attribute is the English fallback, and the `*_tag` value is the YAML key to
look up.

## C++ Runtime API

Runtime translation goes through LVGL's native `lv_translation_*` API. There is
no `lv_i18n_init()` / `lv_i18n_set_locale()` — those were part of the removed
legacy table.

### Translating a string

Wrap a user-facing string literal in `lv_tr()`. It returns the active language's
string for that key, falling back to the English literal if there's no
translation:

```cpp
// Returns the localized string for the active language ("Cancel" in English)
const char* label = lv_tr("Cancel");

// Format strings: translate the format, then fmt::format with the args
std::string msg = fmt::format(lv_tr("Nozzle: {}°C"), temp);
```

Wrapping a C++ string literal in `lv_tr(...)` (or `fmt::format(lv_tr("..{}.."), arg)`
for format strings) is exactly how a C++ string becomes translatable — the
wrapped literal is also picked up by `make translation-sync`.

### Loading a locale and changing language

The active locale's pack is loaded on demand and made active. The locale loader
registers the per-locale XML pack, and `lv_translation_set_language()` switches
the active language for all `lv_tr()` lookups:

```cpp
// In system_settings_manager.cpp / application.cpp
helix::ui::ensure_translation_loaded(lang);   // registers ui_xml/translations/<lang>.xml
lv_translation_set_language(lang.c_str());     // switch active language
lv_obj_invalidate(lv_screen_active());         // refresh visible UI
```

See `src/system/translation_loader.cpp` for the loader, which tracks which
locales have already been registered so a pack is parsed at most once.

## Plural Rules

Plural forms follow the [CLDR standard](https://unicode-org.github.io/cldr-staging/charts/latest/supplemental/language_plural_rules.html).

### Categories

| Category | Typical Usage |
|----------|---------------|
| `zero`   | Zero items (some languages only) |
| `one`    | Singular (1 item, or special cases) |
| `two`    | Dual (exactly 2, some languages) |
| `few`    | Paucal (2-4 in Slavic languages) |
| `many`   | Large numbers (5+ in Slavic languages) |
| `other`  | General plural / fallback |

### Language Examples

**English** (simple - one/other):
- 1 → one ("1 item")
- 0, 2, 3... → other ("0 items", "2 items")

**French** (0/1 singular):
- 0, 1 → one ("0 élément", "1 élément")
- 2, 3... → other ("2 éléments")

**Russian** (complex - one/few/many/other):
- 1, 21, 31... → one ("1 элемент")
- 2-4, 22-24... → few ("2 элемента")
- 5-20, 25-30... → many ("5 элементов")
- Decimals → other

### Generated Plural Functions

Each language gets a plural function implementing its rules:

```c
// English: simple one/other
static uint8_t en_plural_fn(int32_t num) {
    uint32_t i = op_i(op_n(num));
    uint32_t v = op_v(op_n(num));

    if ((i == 1 && v == 0)) return LV_I18N_PLURAL_TYPE_ONE;
    return LV_I18N_PLURAL_TYPE_OTHER;
}

// Russian: one/few/many/other
static uint8_t ru_plural_fn(int32_t num) {
    uint32_t i = op_i(op_n(num));
    uint32_t i10 = i % 10;
    uint32_t i100 = i % 100;

    if ((v == 0 && i10 == 1 && i100 != 11))
        return LV_I18N_PLURAL_TYPE_ONE;
    if ((v == 0 && (2 <= i10 && i10 <= 4) && (!(12 <= i100 && i100 <= 14))))
        return LV_I18N_PLURAL_TYPE_FEW;
    if ((v == 0 && i10 == 0) || (v == 0 && (5 <= i10 && i10 <= 9)) ||
        (v == 0 && (11 <= i100 && i100 <= 14)))
        return LV_I18N_PLURAL_TYPE_MANY;
    return LV_I18N_PLURAL_TYPE_OTHER;
}
```

## Developer Workflow

### Adding New Translatable Strings

1. **Add to XML with translation_tag:**
   ```xml
   <text_body text="New Feature" translation_tag="New Feature"/>
   ```

2. **Extract to YAML files:**
   ```bash
   make translation-sync
   ```
   This scans XML files and adds new keys to all YAML files with the English value as placeholder.

3. **Translate in YAML files:**
   Edit each `translations/*.yml` file to add proper translations:
   ```yaml
   # translations/de.yml
   "New Feature": "Neue Funktion"

   # translations/fr.yml
   "New Feature": "Nouvelle fonctionnalité"
   ```

   When translating a recurring domain term, reuse the rendering in
   `translations/GLOSSARY.md` for that language. For terms not in the glossary,
   grep the target `translations/<lang>.yml` and reuse the dominant existing
   rendering — never coin a new word for a concept that's already translated.

4. **Regenerate the XML translation packs:**
   ```bash
   make translations
   ```

5. **Rebuild and test:**
   ```bash
   make -j && ./build/bin/helix-screen --test -vv
   ```

### Preview Changes Before Sync

```bash
make translation-sync-dry-run
```

This shows what keys would be added without modifying files.

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make translations` | Regenerate the XML translation packs from YAML |
| `make translation-sync` | Extract strings from XML, merge new keys to YAML |
| `make translation-sync-dry-run` | Preview sync without modifying files |
| `make translation-coverage` | Show translation coverage statistics |
| `make translation-glossary` | Regenerate `translations/GLOSSARY.md` from the YAML |
| `make translation-obsolete` | Find translation keys not used in XML |

## Key Files Reference

| File | Purpose |
|------|---------|
| `translations/*.yml` | Source translation files (one per language) |
| `translations/GLOSSARY.md` | Canonical per-language renderings of recurring terms (generated) |
| `scripts/generate_translations.py` | Main pack generator |
| `scripts/translation_sync.py` | String extraction, sync, and glossary tool |
| `scripts/translations/` | Support modules for sync tool |
| `mk/translations.mk` | Makefile integration |
| `ui_xml/translations/translations.xml` | Generated combined LVGL XML pack |
| `ui_xml/translations/<lang>.xml` | Generated per-locale packs (loaded at runtime) |
| `src/system/translation_loader.cpp` | Per-locale pack loader (`ensure_translation_loaded()`) |
| `src/application/application.cpp` | Translation startup / language switching |
| `src/system/settings_manager.cpp` | Locale switching |

## Troubleshooting

### Translation not appearing

1. Check the key exists in `translations/en.yml`
2. Verify `translation_tag` matches the key exactly
3. Run `make translations` to regenerate
4. Rebuild the application

### Missing translation warning

The generator warns about keys missing in non-English locales:
```
WARNING: Missing 'New Feature' in de
WARNING: Missing 'New Feature' in fr
```

Fill in missing translations in the appropriate YAML files.

### Plural form not working

1. Verify the key uses dictionary format in YAML
2. Check the language has a plural rule in `generate_translations.py`
3. Ensure you're using the correct plural categories for that language
