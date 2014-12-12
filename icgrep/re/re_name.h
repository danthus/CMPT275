#ifndef RE_NAME_H
#define RE_NAME_H

#include <re/re_re.h>
#include <re/re_cc.h>
#include <string>
#include <iostream>
#include <re/printer_re.h>

namespace pablo {
    class Var;
}


namespace re {

class Name : public RE {
public:
    static inline bool classof(const RE * re) {
        return re->getClassTypeId() == ClassTypeId::Name;
    }
    static inline bool classof(const void *) {
        return false;
    }
    enum class Type {
        ASCII
        , Unicode
        , UnicodeCategory
        , Unknown
    };
    const std::string & getName() const;
    Type getType() const;
    RE *getDefinition() const;
    pablo::Var * getCompiled() const {
        return mCompiled;
    }
    void setCompiled(pablo::Var * var) {
        mCompiled = var;
    }
    void setDefinition(RE * def);
    virtual ~Name() {}
protected:
    friend Name * makeName(const std::string, RE *);
    friend Name * makeName(const std::string, const Type);
    void* operator new (std::size_t size) noexcept {
        return mAllocator.allocate(size);
    }
    Name(const std::string && name, const Type type, RE * defn)
    : RE(ClassTypeId::Name)
    , mName(std::move(name))
    , mType(type)
    , mDefiningRE(defn)
    , mCompiled(nullptr)
    {

    }

private:
    const std::string   mName;
    const Type          mType;
    RE *                mDefiningRE;
    pablo::Var *        mCompiled;
};

inline const std::string & Name::getName() const {
    return mName;
}

inline Name::Type Name::getType() const {
    return mType;
}

inline RE * Name::getDefinition() const {
    return mDefiningRE;
}

inline void Name::setDefinition(RE * d) {
    mDefiningRE = d;
}

inline Name * makeName(const std::string name, const Name::Type type = Name::Type::Unicode) {
    return new Name(std::move(name), type, nullptr);
}

inline Name * makeName(const std::string name, RE * cc) {
    if (isa<Name>(cc)) {
        return cast<Name>(cc);
    }
    else if (isa<CC>(cc)) {
        Name::Type ccType = cast<CC>(cc)->max_codepoint() <= 0x7F ? Name::Type::ASCII : Name::Type::Unicode;
        return new Name(std::move(name), ccType, cc);
    }
    else return new Name(std::move(name), Name::Type::Unknown, cc);
}

}

#endif // RE_NAME_H
