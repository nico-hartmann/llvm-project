// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
// Declares llvm::cl::extrahelp.
#include "llvm/Support/CommandLine.h"

#include <fstream>
#include <iostream>
#include <unordered_map>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

static const char* TorqueableFriendMarker = "tq::Torque";
static const char* Starline = "****************************************\n";

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("v8-wrench options");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nMore help text...\n");

struct ClassAnnotation {
  std::string name;
  std::vector<std::string> arguments;
};

struct ClassData {
  struct Field {
    std::string type;
    std::string name;
  };

  std::vector<ClassAnnotation> class_annotations;
  std::string name;
  std::string base_class;
  std::vector<Field> fields;
  const CXXRecordDecl* declaration_node;
  std::string source_file;

  explicit ClassData(const CXXRecordDecl* declaration, std::string source_file)
    : declaration_node(declaration), source_file(std::move(source_file)) {}
};

auto TorqueableClassMatcher
  = cxxRecordDecl(
      has(
        friendDecl(
          hasType(
            cxxRecordDecl(
              hasName(TorqueableFriendMarker)
            )
          )
        )
      )
    ).bind("torqueable-class");

class Wrench : public MatchFinder::MatchCallback {
  public:
    int CollectTorqueableClasses(CommonOptionsParser& options_parser);
    void ProcessClasses();
    void GenerateTorqueClasses(const std::string& output_directory);

    bool tracing() const { return true; }
    auto& trace() { return llvm::outs(); }
    auto& errs() { return llvm::errs(); }

  protected:
    virtual void run(const MatchFinder::MatchResult& result) override;
    std::vector<ClassAnnotation> ProcessClassAnnotations(
      const TemplateSpecializationType* type);
    void GenerateTorqueClass(const ClassData& cls, std::ostream& stream);
    std::string MakeSnakeCase(const StringRef& str);
    std::string MakeCamelCase(const StringRef& str);
    void PrintFileHeader(std::ostream& stream, const std::string& source_file);
 
  private:
    std::vector<ClassData> classes_;
};

void Wrench::run(const MatchFinder::MatchResult& result) {
  SourceManager& source_manager = *result.SourceManager;
  const CXXRecordDecl* torqueable_class
    = result.Nodes.getNodeAs<CXXRecordDecl>("torqueable-class");
  if(!torqueable_class) return ;
  if(classes_.end() != std::find_if(classes_.begin(), classes_.end(),
      [&](const auto& cd) { return cd.declaration_node == torqueable_class; })) {
    errs() << "Class '"
    << torqueable_class->getNameAsString() << "' ("
    << torqueable_class->getLocation().printToString(source_manager)
    << ") found multiple times!\n";
    return;
  } 

  std::string source_file
    = torqueable_class->getLocation().printToString(source_manager);
  trace() << "\n* Class '"
     << torqueable_class->getNameAsString() << "': "
     << source_file << "\n";
     classes_.emplace_back(torqueable_class, std::move(source_file));
}

std::string Wrench::MakeSnakeCase(const StringRef& str) {
  std::string result;
  for(size_t i = 0; i < str.size(); ++i) {
    if(str[i] >= 'A' && str[i] <= 'Z') {
      const char lower = str[i] + ('a' - 'A');
      if(!result.empty()) result.push_back('_');
      result.push_back(lower);
    } else {
      result.push_back(str[i]);
    }
  }
  return result;
}

std::string Wrench::MakeCamelCase(const StringRef& str) {
  std::string result;
  bool next_capital = false;
  if(str.empty()) return "";
  std::size_t i = 0;
  if(str[i] >= 'A' && str[i] <= 'Z') {
    result.push_back(str[i] + ('a' - 'A'));
    ++i;
  }
  for( ; i < str.size(); ++i) {
    if(str[i] == '_') next_capital = true;
    else if(next_capital && str[i] >= 'a' && str[i] <= 'z') {
      result.push_back(str[i] + ('A' - 'a'));
      next_capital = false;
    } else {
      result.push_back(str[i]);
      next_capital = false;
    }
  }
  return result;
}

void Wrench::PrintFileHeader(std::ostream& stream, const std::string& source_file) {
  stream <<
    "// This file is automatically generated by v8-wrench.\n"
    "//\n"
    "// WARNING: All modifications to this file will be lost with the next build!\n"
    "//\n"
    "// Source file: " << source_file << "\n"
    "//\n\n";
}

std::vector<ClassAnnotation> Wrench::ProcessClassAnnotations(
    const TemplateSpecializationType* type) {

  const TemplateDecl* decl = type->getTemplateName().getAsTemplateDecl();
  if(decl->getNameAsString() != "Torque") {
    errs() << "Not a 'Torque' marker: " << decl->getNameAsString() << "\n";
    return {};
  }

  trace() << "Annotations:\n";

  std::vector<ClassAnnotation> annotations;
  auto template_args = type->template_arguments();
  for(std::size_t i = 0; i < template_args.size(); ++i) {
    ClassAnnotation annotation;
    const auto arg_type = template_args[i].getAsType()->getUnqualifiedDesugaredType();
    const std::string annotation_string = template_args[i].getAsType().getAsString();
    trace() << " - " << annotation_string << ":\n";

    // Check that this annotation is a record.
    if(!arg_type->isRecordType()) {
      errs() << "Class annotation '" << annotation_string << "' is not a record";
      continue;
    }
    const RecordType* r_type = dyn_cast<RecordType>(arg_type);
    const RecordDecl* r_decl = r_type->getAsRecordDecl();

    // Check that this annotation is in 'tq' namespace.
    if(!r_decl->getEnclosingNamespaceContext()->isNamespace() ||
      dyn_cast<NamespaceDecl>(r_decl->getEnclosingNamespaceContext())->getName() != "tq") {
      errs() << "Class annotation '" << annotation_string << "' is not in 'tq' namespace";
      continue;
    }

    annotation.name = r_decl->getNameAsString();
    trace() << "  - name: " << annotation.name << "\n";

    if(const ClassTemplateSpecializationDecl* cts_type = dyn_cast<ClassTemplateSpecializationDecl>(r_decl)) {
      for(std::size_t arg_index = 0; arg_index < cts_type->getTemplateArgs().size(); ++arg_index) {
        const auto& arg = cts_type->getTemplateArgs().get(arg_index);
        trace() << "  - arguments:\n";
        if(arg.getKind() == TemplateArgument::ArgKind::Integral) {
          const auto integral_arg = arg.getAsIntegral();
          const int64_t value = integral_arg.getExtValue();
          trace() << "   [" << arg_index << "]: " << value << " (integral)\n";
          annotation.arguments.push_back(std::to_string(value));
        } else {
          errs() << "Class annotation '" << annotation_string << "' contains unexpected template argument kind: " <<
            arg.getKind() << "\n";
        }
      }
    }
 
    annotations.push_back(std::move(annotation));
  }

  return annotations;
}

void Wrench::ProcessClasses() {
  trace() << "\n" << Starline;
  trace() << "Processing torqueable classes...\n";
  for(auto& cls : classes_) {
    cls.name = cls.declaration_node->getNameAsString();

    trace() << "\n* Class '" << cls.name << "':\n";

    for(const auto& f : cls.declaration_node->friends()) {
      QualType f_type = f->getFriendType()->getType();

      if(const ElaboratedType* elab = dyn_cast<ElaboratedType>(f_type)) {
        if(const auto* ts_type = dyn_cast<TemplateSpecializationType>(elab->desugar())) {
          cls.class_annotations = ProcessClassAnnotations(ts_type);
        } else {
          errs() << "Friend declaration is not a TemplateSpecializationType\n";
          continue;
        }
      } else {
        errs() << "Friend declaration is not an ElaboratedType\n";
        continue;
      }

    }

    // Process base classes.
    trace() << "Base class: ";
    if(!cls.declaration_node->bases().empty()) {
      auto it = cls.declaration_node->bases().begin();
      const CXXBaseSpecifier& base = *it;
      if(base.getType()->isRecordType()) {
        const CXXRecordDecl* base_decl = base.getType()->getAsCXXRecordDecl();
        assert(base_decl);
        cls.base_class = base_decl->getNameAsString();
        trace() << cls.base_class << "\n";
      } else {
        errs() << "Base is not a record type\n";
      }
      assert(++it == cls.declaration_node->bases().end());
    }

    trace() << "Detected field offsets:\n";
    for(const Decl* inner_decl : cls.declaration_node->decls()) {
      // Discard non-variable declarations.
      if(inner_decl->getKind() != Decl::Kind::Var) continue;

      const VarDecl* var_decl = static_cast<const VarDecl*>(inner_decl);

      // Discard non-static members.
      if(!var_decl->isStaticDataMember()) continue;
      
      // Discard declarations not named k...Offset
      StringRef var_name = var_decl->getName();
      if(!(var_name.startswith("k") && var_name.endswith("Offset"))) continue;
    
      const auto* elab_type = dyn_cast<ElaboratedType>(var_decl->getType());
      // Discard non-elaborated types.
      if(!elab_type) continue;

      const auto* ts_type = dyn_cast<TemplateSpecializationType>(
        elab_type->getQualifier()->getAsType());
      // Discard non-templated types.
      if(!ts_type) continue;
      
      TemplateName template_name = ts_type->getTemplateName();
      const TemplateDecl* template_decl = template_name.getAsTemplateDecl();
      assert(template_decl);
      // Discard non-"Field" declarations.
      if(template_decl->getNameAsString() != "Field") continue;

      // Discard fields with missing type argument.
      const auto args = ts_type->template_arguments();
      if(args.size() < 1) continue;

      // Process field type.
      ClassData::Field field;
      const auto arg0_type = args[0].getAsType().getUnqualifiedType();
      if(arg0_type->isRecordType()) {
        const RecordDecl* record_decl = arg0_type->getAsRecordDecl();
        field.type = record_decl->getNameAsString();
      } else if(arg0_type->isBuiltinType()) {
        const BuiltinType* builtin_type = dyn_cast<BuiltinType>(arg0_type);
        switch(builtin_type->getKind()) {
          case BuiltinType::Kind::Double:
            field.type = "float64";
            break;
          default:
            assert(false);
        }
      } else {
        errs() << "Type '" << arg0_type.getAsString() << "' of declaration '"
          << var_name << "' cannot be handled";
        continue;
      }               
      
      // Strip optional underscore prefix.
      assert(field.type.length() > 0);
      if(field.type[0] == '_') field.type = field.type.substr(1);

      StringRef field_name
        = var_name.drop_front(strlen("k")).drop_back(strlen("Offset"));
      field.name = MakeSnakeCase(field_name);

      trace() << " - '" << field.name << "':\n";
      trace() << "  - type: " << field.type << "\n";
      
      cls.fields.push_back(std::move(field));
    }
  }
}


void Wrench::GenerateTorqueClasses(const std::string& output_directory) {
  trace() << "\n" << Starline;
  trace() << "Generating Torque Classes...\n";
  for(auto cls : classes_) {

    assert(!output_directory.empty() && output_directory.back() != '/');
    const std::string output_filename = output_directory + "/" + MakeSnakeCase(cls.name) + ".tq";

    if(tracing()) {
      trace() << "\n* Class '" << cls.name << "' (" << output_filename << ")\n";
      GenerateTorqueClass(cls, std::cout);
    }

    std::ofstream file;
    file.open(output_filename, std::ios::out | std::ios::trunc);
    if(!file.is_open()) {
      errs() << "Failed to open file '" << output_filename << "'\n";
      continue;
    }

    PrintFileHeader(file, cls.source_file);
    GenerateTorqueClass(cls, file);

  }
}

void Wrench::GenerateTorqueClass(const ClassData& data, std::ostream& stream) {
  // Class header.
  for(const ClassAnnotation& annotation : data.class_annotations) {
    stream << "@" << MakeCamelCase(annotation.name);
    if(!annotation.arguments.empty()) {
      stream << "(";
      for(std::size_t i = 0; i < annotation.arguments.size(); ++i) {
        if(i != 0) stream << ", ";
        stream << annotation.arguments[i];
      }
      stream << ")";
    }
    stream << "\n";
  }
  stream << "class " << data.name;
  if(!data.base_class.empty()) {
    stream << " extends " << data.base_class;
  }
  stream << " {\n";
  
  // Class fields.
  for(const ClassData::Field& field : data.fields) {
    stream << "  " << field.name << ": " << field.type << ";\n";
  }

  stream << "}\n";
}

int Wrench::CollectTorqueableClasses(CommonOptionsParser& options_parser) {
  ClangTool tool(options_parser.getCompilations(),
  options_parser.getSourcePathList());

  MatchFinder finder;
  finder.addMatcher(TorqueableClassMatcher, this);

  trace() << "\n" << Starline;
  trace() << "Searching torqueable classes...\n";
  if(int result = tool.run(newFrontendActionFactory(&finder).get())) {
    errs() << "Collecting torqueable classes failed";
    return result;
  }

  return 0;
}



int main(int argc, const char **argv) {
  Wrench wrench;

  // Parse command line.
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
  if (!ExpectedParser) {
    // Fail gracefully for unsupported options.
    wrench.errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& options_parser = ExpectedParser.get();

  // Collect torqueable classes.
  if(int result = wrench.CollectTorqueableClasses(options_parser))
    return result;
  wrench.ProcessClasses();

  const std::string output_dir = "src/objects";
  wrench.GenerateTorqueClasses(output_dir);

  return 0;
}
