# Interface

## interface UnicodeRuneExtension

```cangjie
public interface UnicodeRuneExtension
```

Function: Interface for Unicode character set related extensions.

Can be used to add a series of Unicode character set related extension functions to the [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) type, including character type determination, case conversion, whitespace removal, etc.

Extensions:

- [extend Rune <: UnicodeRuneExtension](unicode_package_extends.md#extend-rune--unicoderuneextension) (cross-package extension)

### func isLetter()

```cangjie
func isLetter(): Bool
```

Function: Determines whether the type is a Unicode letter character.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the type is a Unicode letter character, otherwise returns `false`.

### func isLowerCase()

```cangjie
func isLowerCase(): Bool
```

Function: Determines whether the type is a Unicode lowercase character.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the type is a Unicode lowercase character, otherwise returns `false`.

### func isNumber()

```cangjie
func isNumber(): Bool
```

Function: Determines whether the type is a Unicode numeric character.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the type is a Unicode numeric character, otherwise returns `false`.

### func isTitleCase()

```cangjie
func isTitleCase(): Bool
```

Function: Determines whether the type is a Unicode titlecase character.

Titlecase characters in Unicode refer to special letter forms used in certain languages to represent the capitalization of the first letter of each word in titles. These characters are represented by special codes, such as U+01C5 (ǅ) and U+01F1 (Ǳ). They are commonly used in some Eastern European languages like Croatian and Serbian.

Titlecase characters include: `0x01C5`, `0x01C8`, `0x01CB`, `0x01F2`, `0x1F88 - 0x1F8F`, `0x1F98 - 0x1F9F`, `0x1F98 - 0x1F9F`, `0x1FA8 - 0x1FAF`, `0x1FBC`, `0x1FCC`, `0x1FFC`

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the type is a Unicode titlecase character, otherwise returns `false`.

### func isUpperCase()

```cangjie
func isUpperCase(): Bool
```

Function: Determines whether the type is a Unicode uppercase character.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the type is a Unicode uppercase character, otherwise returns `false`.

### func isWhiteSpace()

```cangjie
func isWhiteSpace(): Bool
```

Function: Determines whether the type is a Unicode whitespace character.

Whitespace characters include: `0x0009`, `0x000A`, `0x000B`, `0x000C`, `0x000D`, `0x0020`, `0x0085`, `0x00A0`, `0x1680`, `0x2000`, `0x2001`, `0x2002`, `0x2003`, `0x2004`, `0x2005`, `0x2006`, `0x2007`, `0x2008`, `0x2009`, `0x200A`, `0x2028`, `0x2029`, `0x202F`, `0x205F`, `0x3000`.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the type is a Unicode whitespace character, otherwise returns `false`.

### func toLowerCase()

```cangjie
func toLowerCase(): Rune
```

Function: Gets the corresponding Unicode lowercase character for this type.

Returns:

- [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - The lowercase character corresponding to the current type.

### func toTitleCase()

```cangjie
func toTitleCase(): Rune
```

Function: Gets the corresponding Unicode titlecase character for this type.

Returns:

- [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - The titlecase character corresponding to the current type.

### func toUpperCase()

```cangjie
func toUpperCase(): Rune
```

Function: Gets the corresponding Unicode uppercase character for this type.

Returns:

- [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - The uppercase character corresponding to the current type.

### func toLowerCase(CasingOption)

```cangjie
func toLowerCase(opt: CasingOption): Rune
```

Function: Gets the corresponding Unicode lowercase character for this type.

Parameters:

- opt: [CasingOption](./unicode_package_enums.md#enum-casingoption) - The input language enumeration.Returns:

- [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - The lowercase character corresponding to the current type.

### func toTitleCase(CasingOption)

```cangjie
func toTitleCase(opt: CasingOption): Rune
```

Function: Gets the `Unicode` title case character corresponding to this type.

Parameters:

- opt: [CasingOption](./unicode_package_enums.md#enum-casingoption) - The input language enumeration.

Returns:

- [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - The title case character corresponding to the current type.

### func toUpperCase(CasingOption)

```cangjie
func toUpperCase(opt: CasingOption): Rune
```

Function: Gets the `Unicode` uppercase character corresponding to this type.

Parameters:

- opt: [CasingOption](./unicode_package_enums.md#enum-casingoption) - The input language enumeration.

Returns:

- [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) - The lowercase character corresponding to the current type.

## interface UnicodeStringExtension

```cangjie
public interface UnicodeStringExtension
```

Function: Interface for `Unicode` character set related extensions.

Can be used to add a series of extension functions related to the `Unicode` character set for the [String](../../core/core_package_api/core_package_structs.md#struct-string) type, including character type determination, case conversion, whitespace removal, etc.

Extensions:

- [extend String <: UnicodeStringExtension](unicode_package_extends.md#extend-string--unicodestringextension) (cross-package extension)

### func isBlank()

```cangjie
func isBlank(): Bool
```

Function: Determines whether the current string is empty or contains only whitespace characters from the `Unicode` character set.

Whitespace characters include `0x0009`, `0x000A`, `0x000B`, `0x000C`, `0x000D`, `0x0020`, `0x0085`, `0x00A0`, `0x1680`, `0x2000`, `0x2001`, `0x2002`, `0x2003`, `0x2004`, `0x2005`, `0x2006`, `0x2007`, `0x2008`, `0x2009`, `0x200A`, `0x2028`, `0x2029`, `0x202F`, `0x205F`, `0x3000`.

Returns:

- [Bool](../../core/core_package_api/core_package_intrinsics.md#bool) - Returns `true` if the string is empty or contains only whitespace characters; otherwise, returns `false`.

### func toLower()

```cangjie
func toLower(): String
```

Function: Converts all uppercase characters within the current string's `Unicode` character set range to lowercase.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted all-lowercase string.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func toLower(CasingOption)

```cangjie
func toLower(opt: CasingOption): String
```

Function: Converts all uppercase characters within the current string's `Unicode` character set range to lowercase.

Parameters:

- opt: [CasingOption](./unicode_package_enums.md#enum-casingoption) - The input language enumeration.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted all-lowercase string.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func toTitle()

```cangjie
func toTitle(): String
```

Function: Converts characters in the current string that can be transformed to title case within the `Unicode` character set range to title case.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted title case string.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func toTitle(CasingOption)

```cangjie
func toTitle(opt: CasingOption): String
```

Function: Converts characters in the current string that can be transformed to title case within the `Unicode` character set range to title case.

Parameters:

- opt: [CasingOption](./unicode_package_enums.md#enum-casingoption) - The input language enumeration.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted title case string.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func toUpper()

```cangjie
func toUpper(): String
```

Function: Converts all lowercase characters within the current string's `Unicode` character set range to uppercase.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted all-uppercase string.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func toUpper(CasingOption)

```cangjie
func toUpper(opt: CasingOption): String
```

Function: Converts all lowercase characters within the current string's `Unicode` character set range to uppercase.

Parameters:

- opt: [CasingOption](./unicode_package_enums.md#enum-casingoption) - The input language enumeration.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The converted all-uppercase string.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func trim()

```cangjie
func trim(): String
```

Function: Removes leading and trailing whitespace characters from the string. Whitespace characters are defined by the extension function [isWhiteSpace](#func-iswhitespace) of the [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) type.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string with leading and trailing whitespace removed.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func trimEnd()

```cangjie
func trimEnd(): String
```

Function: Removes trailing whitespace characters from the string. Whitespace characters are defined by the extension function [isWhiteSpace](#func-iswhitespace) of the [Rune](../../../std_en/core/core_package_api/core_package_intrinsics.md#rune) type.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string with trailing whitespace removed.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Throws an exception if the string contains invalid UTF-8 encoding.

### func trimLeft() <sup>(deprecated)</sup>

```cangjie
func trimLeft(): String
```

Function: Removes leading whitespace characters from the string. Whitespace characters are defined by the extension function [isWhiteSpace](#func-iswhitespace) of the [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) type.

> **Note:**
>
> This function will be deprecated in future versions. Use [trimStart](./unicode_package_interfaces.md#func-trimstart) instead.Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string with leading whitespace characters removed.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown if the string contains invalid UTF-8 encoding.

### func trimRight() <sup>(deprecated)</sup>

```cangjie
func trimRight(): String
```

Function: Removes trailing whitespace characters from the string. Whitespace characters are defined by the extension function [isWhiteSpace](#func-iswhitespace) of the [Rune](../../../std_en/core/core_package_api/core_package_intrinsics.md#rune) type.

> **Note:**
>
> This will be deprecated in future versions. Use [trimEnd](./unicode_package_interfaces.md#func-trimend) instead.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string with trailing whitespace characters removed.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown if the string contains invalid UTF-8 encoding.

### func trimStart()

```cangjie
func trimStart(): String
```

Function: Removes leading whitespace characters from the string. Whitespace characters are defined by the extension function [isWhiteSpace](#func-iswhitespace) of the [Rune](../../core/core_package_api/core_package_intrinsics.md#rune) type.

Returns:

- [String](../../core/core_package_api/core_package_structs.md#struct-string) - The string with leading whitespace characters removed.

Exceptions:

- [IllegalArgumentException](../../core/core_package_api/core_package_exceptions.md#class-illegalargumentexception) - Thrown if the string contains invalid UTF-8 encoding.

