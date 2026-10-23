# std.unicode

## Functionality Overview

The unicode package provides capabilities for processing characters according to the Unicode encoding standard.

`Unicode` is a character encoding standard designed to provide a unified encoding scheme for all languages and symbols, facilitating text exchange and processing in computer systems.

The `Unicode` encoding standard represents each character with a unique code point while defining various properties for each character, such as category (letter, digit, punctuation, etc.), script (Latin, Greek, Hanzi, etc.), case mapping (uppercase or lowercase mapping relationships), and diacritical marks (whether it contains diacritics like accent marks).

This package provides the `UnicodeRuneExtension` and `UnicodeStringExtension` interface types, implementing several extension methods for `Rune` and `String` types, including character type determination and case conversion.

## API List

### Interfaces

|                 Interface Name                |                Functionality                |
| -------------------------------------------- | ------------------------------------------- |
| [UnicodeRuneExtension](./unicode_package_api/unicode_package_interfaces.md#interface-unicoderuneextension) | Interface for Unicode character set-related extensions, used to extend Unicode character set operations for the `Rune` type. |
| [UnicodeStringExtension](./unicode_package_api/unicode_package_interfaces.md#interface-unicodestringextension) | Interface for Unicode character set-related extensions, used to extend Unicode character set operations for the `String` type. |

### Enums

|                 Enum                |                Functionality                |
| ----------------------------------- | ------------------------------------------- |
| [CasingOption](./unicode_package_api/unicode_package_enums.md#enum-casingoption) | Enumeration class required for case conversion based on different languages. |

### Extensions

| Extension Name | Functionality |
| --- | --- |
| [extend Rune <: UnicodeRuneExtension](unicode_package_api/unicode_package_extends.md#extend-rune--unicoderuneextension) | Extends the [Rune](../core/core_package_api/core_package_intrinsics.md#rune) type with the [UnicodeRuneExtension](unicode_package_api/unicode_package_interfaces.md#interface-UnicodeRuneExtension) interface to support character set-related operations. |
| [extend String <: UnicodeStringExtension](unicode_package_api/unicode_package_extends.md#extend-string--unicodestringextension) | Extends the [String](../core/core_package_api/core_package_structs.md#struct-string) type with the [UnicodeRuneExtension](unicode_package_api/unicode_package_interfaces.md#interface-UnicodeStringExtension) interface to support character set-related operations. |
