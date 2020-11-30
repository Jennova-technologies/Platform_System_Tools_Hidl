/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <hidl-util/FQName.h>
#include <hidl-util/Formatter.h>
#include <hidl-util/StringHelper.h>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "AidlHelper.h"
#include "CompoundType.h"
#include "Coordinator.h"
#include "NamedType.h"
#include "ScalarType.h"
#include "Scope.h"

namespace android {

std::string AidlHelper::translateHeaderFile(const FQName& fqName, AidlBackend backend) {
    switch (backend) {
        case AidlBackend::NDK:
            return AidlHelper::getAidlPackagePath(fqName) + "/translate-ndk.h";
        case AidlBackend::CPP:
            return AidlHelper::getAidlPackagePath(fqName) + "/translate-cpp.h";
        default:
            LOG(FATAL) << "Unexpected AidlBackend value";
            return "";
    }
}

std::string AidlHelper::translateSourceFile(const FQName& fqName, AidlBackend backend) {
    switch (backend) {
        case AidlBackend::NDK:
            return AidlHelper::getAidlPackagePath(fqName) + "/translate-ndk.cpp";
        case AidlBackend::CPP:
            return AidlHelper::getAidlPackagePath(fqName) + "/translate-cpp.cpp";
        case AidlBackend::JAVA:
            return AidlHelper::getAidlPackagePath(fqName) + "/Translate.java";
        default:
            LOG(FATAL) << "Unexpected AidlBackend value";
            return "";
    }
}

static const std::string aidlTypePackage(const NamedType* type, AidlBackend backend) {
    const std::string prefix = (backend == AidlBackend::NDK) ? "aidl::" : std::string();
    const std::string separator = (backend == AidlBackend::JAVA) ? "." : "::";
    return prefix +
           base::Join(base::Split(AidlHelper::getAidlPackage(type->fqName()), "."), separator) +
           separator + AidlHelper::getAidlType(*type, type->fqName());
}

static void namedTypeTranslation(Formatter& out, const std::set<const NamedType*>& namedTypes,
                                 const FieldWithVersion& field, const CompoundType* parent,
                                 AidlBackend backend) {
    const NamedType* type = static_cast<const NamedType*>(field.field->get());
    if (namedTypes.find(type) == namedTypes.end()) {
        std::optional<const ReplacedTypeInfo> replacedType =
                AidlHelper::getAidlReplacedType(type->fqName());
        if (replacedType) {
            std::optional<std::function<void(Formatter&)>> translateField =
                    replacedType.value().translateField;
            if (translateField) {
                translateField.value()(out);
            }
        } else {
            AidlHelper::notes() << "An unknown named type was found in translation: "
                                << type->fqName().string() + "\n";
            out << "#error FIXME Unknown type: " << type->fqName().string() << "\n";
        }
    } else {
        if (parent->style() == CompoundType::STYLE_STRUCT) {
            if (backend == AidlBackend::JAVA) {
                out << "out." << field.field->name() << " = h2aTranslate(in." << field.fullName
                    << ");\n";
            } else {
                out << "if (!translate(in." << field.fullName << ", &out->" << field.field->name()
                    << ")) return false;\n";
            }
        } else {
            if (backend == AidlBackend::JAVA) {
                out << "out.set" << StringHelper::Capitalize(field.field->name())
                    << "(h2aTranslate(in." << field.fullName << "()));\n";
            } else {
                out << "{\n";
                out << aidlTypePackage(type, backend) << " " << field.field->name() << ";\n";
                out << "if (!translate(in." << field.fullName + "(), &" << field.field->name()
                    << ")) return false;\n";
                out << "out->set<" << aidlTypePackage(parent, backend) << "::" << field.fullName
                    << ">(" << field.field->name() << ");\n";
                out << "}\n";
            }
        }
    }
}

static void h2aScalarChecks(Formatter& out, const FieldWithVersion& field,
                            const CompoundType* parent, AidlBackend backend) {
    static const std::map<ScalarType::Kind, size_t> kSignedMaxSize{
            {ScalarType::KIND_UINT8, std::numeric_limits<int8_t>::max()},
            {ScalarType::KIND_INT16, std::numeric_limits<int32_t>::max()},
            {ScalarType::KIND_UINT32, std::numeric_limits<int32_t>::max()},
            {ScalarType::KIND_UINT64, std::numeric_limits<int64_t>::max()}};

    const ScalarType* scalarType = field.field->type().resolveToScalarType();
    if (scalarType != nullptr) {
        const auto& it = kSignedMaxSize.find(scalarType->getKind());
        if (it != kSignedMaxSize.end()) {
            out << "// FIXME This requires conversion between signed and unsigned. Change this if "
                   "it doesn't suit your needs.\n";
            std::string functionCall = (parent->style() == CompoundType::STYLE_STRUCT) ? "" : "()";
            if (scalarType->getKind() == ScalarType::KIND_INT16) {
                // AIDL uses an unsigned 16-bit integer(char16_t), so this is signed to unsigned.
                out << "if (in." << field.fullName << functionCall << " < 0) {\n";
            } else {
                std::string affix = (scalarType->getKind() == ScalarType::KIND_UINT64) ? "L" : "";
                out << "if (in." << field.fullName << functionCall << " > " << it->second << affix
                    << " || in." << field.fullName << functionCall << " < 0) {\n";
            }
            if (backend == AidlBackend::JAVA) {
                out.indent([&] {
                    out << "throw new RuntimeException(\"Unsafe conversion between signed and "
                           "unsigned scalars for field: "
                        << field.fullName << "\");\n";
                });
            } else {
                out.indent([&] { out << "return false;\n"; });
            }
            out << "}\n";
        }
    }
}

static std::string wrapToString16(const std::string& payload, AidlBackend backend) {
    if (backend == AidlBackend::CPP) {
        return "String16(" + payload + ".c_str())";
    } else {
        return payload;
    }
}

static std::string wrapStaticCast(const std::string& payload, const Type& type,
                                  const FQName& fqName) {
    static const std::map<std::string, std::string> kAidlBackendScalarTypes{
            {"boolean", "bool"}, {"byte", "int8_t"}, {"char", "char16_t"}, {"int", "int32_t"},
            {"long", "int64_t"}, {"float", "float"}, {"double", "double"}};
    const auto& it = kAidlBackendScalarTypes.find(AidlHelper::getAidlType(type, fqName));
    if (it != kAidlBackendScalarTypes.end()) {
        return "static_cast<" + it->second + ">(" + payload + ")";
    } else {
        return payload;
    }
}

static std::string wrapCppSource(const std::string& payload, const Type& type, const FQName& fqName,
                                 AidlBackend backend) {
    if (type.isString()) {
        return wrapToString16(payload, backend);
    } else {
        return wrapStaticCast(payload, type, fqName);
    }
}

static void simpleTranslation(Formatter& out, const FieldWithVersion& field,
                              const CompoundType* parent, AidlBackend backend) {
    h2aScalarChecks(out, field, parent, backend);
    if (backend == AidlBackend::JAVA) {
        if (parent->style() == CompoundType::STYLE_STRUCT) {
            out << "out." << field.field->name() << " = in." << field.fullName << ";\n";
        } else {
            out << "out.set" << StringHelper::Capitalize(field.fullName) << "(in."
                << field.field->name() << "());\n";
        }
    } else {
        if (parent->style() == CompoundType::STYLE_STRUCT) {
            out << "out->" << field.field->name() << " = "
                << wrapCppSource("in." + field.fullName, *field.field->get(), parent->fqName(),
                                 backend)
                << ";\n";
        } else {
            out << "*out = "
                << wrapCppSource("in." + field.fullName + "()", *field.field->get(),
                                 parent->fqName(), backend)
                << ";\n";
        }
    }
}

static void h2aFieldTranslation(Formatter& out, const std::set<const NamedType*>& namedTypes,
                                const CompoundType* parent, const FieldWithVersion& field,
                                AidlBackend backend) {
    // TODO(b/158489355) Need to support and validate more types like arrays/vectors.
    if (field.field->type().isNamedType()) {
        namedTypeTranslation(out, namedTypes, field, parent, backend);
    } else if (field.field->type().isEnum() || field.field->type().isScalar() ||
               field.field->type().isString()) {
        simpleTranslation(out, field, parent, backend);
    } else {
        AidlHelper::notes() << "An unhandled type was found in translation: "
                            << field.field->type().typeName() << "\n";
        out << "#error FIXME Unhandled type: " << field.field->type().typeName() << "\n";
    }
}

static const std::string declareAidlFunctionSignature(const NamedType* type, AidlBackend backend) {
    if (backend == AidlBackend::JAVA) {
        return "static public " + aidlTypePackage(type, backend) + " h2aTranslate(" +
               type->fullJavaName() + " in)";
    } else {
        return "__attribute__((warn_unused_result)) bool translate(const " + type->fullName() +
               "& in, " + aidlTypePackage(type, backend) + "* out)";
    }
}

static const std::string getPackageFilePath(const NamedType* type) {
    return base::Join(base::Split(type->fqName().package(), "."), "/");
}

static bool typeComesFromInterface(const NamedType* type) {
    const Scope* parent = type->parent();
    while (parent != nullptr) {
        if (parent->isInterface()) {
            return true;
        }
        parent = parent->parent();
    }
    return false;
}

static const std::string hidlIncludeFile(const NamedType* type) {
    if (typeComesFromInterface(type)) {
        return "#include \"" + getPackageFilePath(type) + "/" + type->fqName().version() + "/" +
               type->parent()->fqName().getInterfaceName() + ".h\"\n";
    } else {
        return "#include \"" + getPackageFilePath(type) + "/" + type->fqName().version() +
               "/types.h\"\n";
    }
}

static const std::string aidlIncludeFile(const NamedType* type, AidlBackend backend) {
    const std::string prefix = (backend == AidlBackend::NDK) ? "aidl/" : std::string();
    return "#include \"" + prefix + getPackageFilePath(type) + "/" +
           AidlHelper::getAidlType(*type, type->fqName()) + ".h\"\n";
}

static void emitCppTranslateHeader(
        const Coordinator& coordinator, const FQName& fqName,
        const std::set<const NamedType*>& namedTypes,
        const std::map<const NamedType*, const ProcessedCompoundType>& processedTypes,
        AidlBackend backend) {
    CHECK(backend == AidlBackend::CPP || backend == AidlBackend::NDK);
    std::set<std::string> includes;
    Formatter out =
            coordinator.getFormatter(fqName, Coordinator::Location::DIRECT,
                                     "include/" + AidlHelper::translateHeaderFile(fqName, backend));

    AidlHelper::emitFileHeader(out);
    out << "#pragma once\n\n";
    for (const auto& type : namedTypes) {
        const auto& it = processedTypes.find(type);
        if (it == processedTypes.end()) {
            continue;
        }
        includes.insert(aidlIncludeFile(type, backend));
        includes.insert(hidlIncludeFile(type));
    }
    out << base::Join(includes, "") << "\n\n";

    out << "namespace android::h2a {\n\n";
    for (const auto& type : namedTypes) {
        const auto& it = processedTypes.find(type);
        if (it == processedTypes.end()) {
            continue;
        }
        out << declareAidlFunctionSignature(type, backend) << ";\n";
    }
    out << "\n}  // namespace android::h2a\n";
}

static void emitTranslateSource(
        const Coordinator& coordinator, const FQName& fqName,
        const std::set<const NamedType*>& namedTypes,
        const std::map<const NamedType*, const ProcessedCompoundType>& processedTypes,
        AidlBackend backend) {
    Formatter out = coordinator.getFormatter(fqName, Coordinator::Location::DIRECT,
                                             AidlHelper::translateSourceFile(fqName, backend));
    AidlHelper::emitFileHeader(out);
    if (backend == AidlBackend::JAVA) {
        out << "package " << AidlHelper::getAidlPackage(fqName) + ";\n\n";
        out << "public class Translate {\n";
    } else {
        out << "#include \""
            << AidlHelper::translateHeaderFile((*namedTypes.begin())->fqName(), backend) + "\"\n\n";
        out << "namespace android::h2a {\n\n";
    }
    for (const auto& type : namedTypes) {
        const auto& it = processedTypes.find(type);
        if (it == processedTypes.end()) {
            continue;
        }
        CHECK(type->isCompoundType()) << "Unexpected type: " << type->fqName().string();
        const CompoundType* compound = static_cast<const CompoundType*>(type);

        if (compound->style() == CompoundType::STYLE_UNION) {
            // HIDL Java backend doesn't support union so don't add a comment.
            if (backend != AidlBackend::JAVA) {
                out << "// FIXME not enough information to safely convert. Remove this function or "
                       "fill it out using the custom discriminators.\n";
                out << "// " << declareAidlFunctionSignature(type, backend) << "\n\n";
            }
            continue;
        }

        out << declareAidlFunctionSignature(type, backend) << " {\n";
        if (compound->style() == CompoundType::STYLE_SAFE_UNION) {
            out.indent([&] {
                if (backend == AidlBackend::JAVA) {
                    out << aidlTypePackage(type, backend) << " out = new "
                        << aidlTypePackage(type, backend) << "();\n";
                }
                out << "switch (in.getDiscriminator()) {\n";
                out.indent([&] {
                    const ProcessedCompoundType& processedType = it->second;
                    for (const auto& field : processedType.fields) {
                        if (backend == AidlBackend::JAVA) {
                            out << "case " << compound->fullJavaName() << ".hidl_discriminator."
                                << field.field->name() << ":\n";
                        } else {
                            out << "case " << compound->fullName()
                                << "::hidl_discriminator::" << field.field->name() << ":\n";
                        }
                        out.indent([&] {
                            h2aFieldTranslation(out, namedTypes, compound, field, backend);
                            out << "break;\n";
                        });
                    }
                    out << "default:\n";
                    if (backend == AidlBackend::JAVA) {
                        out.indent([&] {
                            out << "throw new RuntimeException(\"Unknown discriminator value: \" + "
                                   "Integer.toString(in.getDiscriminator()));\n";
                        });
                    } else {
                        out.indent([&] { out << "return false;\n"; });
                    }
                });
                out << "}\n";
            });
        } else {
            out.indent([&] {
                if (backend == AidlBackend::JAVA) {
                    out << aidlTypePackage(type, backend) << " out = new "
                        << aidlTypePackage(type, backend) << "();\n";
                }
                const ProcessedCompoundType& processedType = it->second;
                for (const auto& field : processedType.fields) {
                    h2aFieldTranslation(out, namedTypes, compound, field, backend);
                }
            });
        }
        if (backend == AidlBackend::JAVA) {
            out.indent([&] { out << "return out;\n"; });
        } else {
            out.indent([&] { out << "return true;\n"; });
        }
        out << "}\n\n";
    }
    if (backend == AidlBackend::JAVA) {
        out << "}";
    } else {
        out << "}  // namespace android::h2a";
    }
}

void AidlHelper::emitTranslation(
        const Coordinator& coordinator, const FQName& fqName,
        const std::set<const NamedType*>& namedTypes,
        const std::map<const NamedType*, const ProcessedCompoundType>& processedTypes) {
    if (processedTypes.empty()) return;
    for (auto backend : {AidlBackend::NDK, AidlBackend::CPP, AidlBackend::JAVA}) {
        if (backend != AidlBackend::JAVA) {
            emitCppTranslateHeader(coordinator, fqName, namedTypes, processedTypes, backend);
        }
        emitTranslateSource(coordinator, fqName, namedTypes, processedTypes, backend);
    }
}

}  // namespace android
