package std.unittest.common

import std.collection.HashMap
import std.ast.*

protected struct MacroInfo {
    public MacroInfo(let instrumentationFunc: (Tokens) -> Tokens) {}
}

public class MacroRegistry {
    private static let registerdMacros: HashMap<String, MacroInfo> = HashMap()

    public static func registerMacro(macroName: String, instrumentationFunc: (Tokens) -> Tokens): Unit {
        registerdMacros.add(macroName, MacroInfo(instrumentationFunc))
    }

    protected static func macroNames(): Iterator<String> {
        registerdMacros.keys().iterator()
    }

    protected static func getMacroInfo(macroName: String): MacroInfo {
        match (registerdMacros.get(macroName)) {
            case None => throw IllegalArgumentException("No such registered unittest macro: ${macroName}")
            case Some(info) => info
        }
    }
}
