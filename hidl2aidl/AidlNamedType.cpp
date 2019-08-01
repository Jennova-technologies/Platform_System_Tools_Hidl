/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "AidlHelper.h"
#include "CompoundType.h"
#include "Coordinator.h"
#include "EnumType.h"
#include "NamedType.h"
#include "TypeDef.h"

namespace android {

static void emitTypeDefAidlDefinition(Formatter& out, const TypeDef& typeDef) {
    out << "// Cannot convert typedef " << typeDef.referencedType()->definedName() << " "
        << typeDef.fqName().string() << " since AIDL does not support typedefs.\n";
}

static void emitEnumAidlDefinition(Formatter& out, const EnumType& enumType) {
    out << "// Cannot convert enum " << enumType.fqName().string()
        << " since AIDL does not support enums.\n";
}

static void emitCompoundTypeAidlDefinition(Formatter& out, const CompoundType& compoundType,
                                           const Coordinator& coordinator) {
    for (const NamedType* namedType : compoundType.getSubTypes()) {
        AidlHelper::emitAidl(*namedType, coordinator);
    }

    compoundType.emitDocComment(out);
    out << "parcelable " << compoundType.definedName() << " ";
    if (compoundType.style() == CompoundType::STYLE_STRUCT) {
        out.block([&] {
            for (const NamedReference<Type>* field : compoundType.getFields()) {
                field->emitDocComment(out);
                // TODO: nested types now should not be referenced as nested types. instead they
                // should be refered to by their direct (fully qualified) name.
                out << AidlHelper::getAidlType(*field->get()) << " " << field->name() << ";\n";
            }
        });
    } else {
        out << "{}\n";
        out << "// FIXME: Add union/safe_union implementations";
    }
    out << "\n\n";
}

// TODO: Enum/Typedef should just emit to hidl-error.log or similar
void AidlHelper::emitAidl(const NamedType& namedType, const Coordinator& coordinator) {
    Formatter out =
            coordinator.getFormatter(namedType.fqName(), Coordinator::Location::GEN_SANITIZED,
                                     getAidlName(namedType) + ".aidl");

    emitFileHeader(out, namedType);

    if (namedType.isTypeDef()) {
        const TypeDef& typeDef = static_cast<const TypeDef&>(namedType);
        emitTypeDefAidlDefinition(out, typeDef);
    } else if (namedType.isCompoundType()) {
        const CompoundType& compoundType = static_cast<const CompoundType&>(namedType);
        emitCompoundTypeAidlDefinition(out, compoundType, coordinator);
    } else if (namedType.isEnum()) {
        const EnumType& enumType = static_cast<const EnumType&>(namedType);
        emitEnumAidlDefinition(out, enumType);
    } else {
        out << "// TODO: Fix this " << namedType.definedName() << "\n";
    }
}

}  // namespace android