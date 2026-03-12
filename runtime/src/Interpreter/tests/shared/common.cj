package common

/**
 * Calls to interpreter mock lib.
 * Behaviour of each function is defined inside mock cpp files. 
 */
public open class C2I {
    public open func invoke0(): Unit {}
    public open func invoke1(): Unit {}
    public open func invoke2(): Unit {}
    public open func invoke3(): Unit {}
    public open func invoke4(): Unit {}
    public open func invoke5(): Unit {}
    public open func invoke6(): Unit {}
    public open func invoke7(): Unit {}
    public open func invoke8(): Unit {}
    public open func invoke9(): Unit {}
}

public var c2i: C2I = C2I()
