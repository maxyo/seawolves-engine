// Script attribute names: the codec numbers every name as it always did (legacy_string_codec.hpp, the numbering the
// saves depend on), and ATTRIBUTES finds, creates, renames, sorts and deletes children as a plain tree that compares
// names one by one does. Built with STORM_VM_VERIFY: every lookup (the scan of a small object's codes, the hash index
// of a large one) also checks itself inside the engine code.
#include "attributes.h"
#include "legacy_string_codec.hpp"
#include "string_codec.h"
#include "string_compare.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

void STRING_CODEC::VariableChanged()
{
}

namespace
{
int failures = 0;

void Fail(const std::string &what)
{
    if (++failures <= 20)
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
}

std::string RandomName(std::mt19937 &random)
{
    // Letters in both cases (the codec folds ASCII only), digits, the separators scripts use, cp1251 and UTF-8 bytes.
    static const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ -\xe0\xc0\xd0\xb0\xff";
    std::string name(random() % 6 == 0 ? random() % 40 : 1 + random() % 9, ' ');
    for (char &c : name)
        c = alphabet[random() % alphabet.size()];
    return name;
}

std::string FlipCase(std::string name, std::mt19937 &random)
{
    for (char &c : name)
        if (random() % 2)
            c = (c >= 'a' && c <= 'z') ? c - 32 : (c >= 'A' && c <= 'Z') ? c + 32 : c;
    return name;
}

void CheckCodec()
{
    std::mt19937 random(20261116);
    for (int round = 0; round < 2; round++) // the second round after Release()
    {
        STRING_CODEC codec;
        LegacyStringCodec legacy;
        std::vector<std::string> seen;
        for (int n = 0; n < 150000; n++)
        {
            std::string name = !seen.empty() && random() % 3 == 0 ? FlipCase(seen[random() % seen.size()], random)
                                                                 : RandomName(random);
            const bool known = [&] {
                bool isNew = false;
                // Asking the legacy codec registers the name, so the answer comes from its count.
                const uint32_t before = legacy.GetNum();
                const uint32_t code = legacy.Convert(name.c_str(), isNew);
                (void)code;
                return legacy.GetNum() == before;
            }();
            if (!known)
            {
                // New to the legacy codec: Find must not know it yet either.
                if (codec.Find(name))
                    Fail("Find knows the unregistered name '" + name + "'");
            }
            uint32_t code;
            const uint32_t expected = legacy.Convert(name.c_str());
            if (random() % 4 == 0)
            {
                // A prefix of a longer buffer.
                const std::string buffer = name + "tail";
                code = codec.Convert(buffer.c_str(), static_cast<int32_t>(name.size()));
            }
            else
                code = codec.Convert(name.c_str());
            if (code != expected)
                Fail("code of '" + name + "': " + std::to_string(code) + ", legacy " + std::to_string(expected));
            if (codec.Find(name) != std::optional<uint32_t>(expected))
                Fail("Find of the registered name '" + name + "'");
            if (std::string(codec.Convert(code)) != legacy.Convert(expected))
                Fail("name of code " + std::to_string(code));
            if (codec.GetNum() != legacy.GetNum())
                Fail("count after '" + name + "'");
            seen.push_back(std::move(name));
        }
        // The order a save writes the names in.
        const char *mine = codec.Get();
        const char *theirs = legacy.Get();
        uint32_t walked = 0;
        while (mine && theirs)
        {
            if (std::string(mine) != theirs)
            {
                Fail(std::string("save order: '") + mine + "' where the legacy codec has '" + theirs + "'");
                break;
            }
            walked++;
            mine = codec.GetNext();
            theirs = legacy.GetNext();
        }
        if (mine || theirs || walked != legacy.GetNum())
            Fail("save order: " + std::to_string(walked) + " names walked of " + std::to_string(legacy.GetNum()));
        if (std::string(codec.Convert(0xffffffffu)) != legacy.Convert(0xffffffffu) ||
            std::string(codec.Convert(0x0000ffffu)) != legacy.Convert(0x0000ffffu))
            Fail("the text of an invalid code");
        codec.Release();
        if (codec.GetNum() != 0 || codec.Get() != nullptr || codec.Find(seen.front()))
            Fail("Release left names");
        if (codec.Convert(static_cast<const char *>(nullptr)) != 0xffffffffu || codec.Convert(nullptr, 3) != 0xffffffffu)
            Fail("the code of a null name");
    }
}

// The plain tree: children in creation order, a name looked up by comparing every child's (ASCII case folded).
struct Node
{
    std::string name;
    std::optional<std::string> value;
    std::vector<std::unique_ptr<Node>> children;

    Node *Child(const std::string &childName) const
    {
        for (const auto &child : children)
            if (storm::iEquals(child->name, childName))
                return child.get();
        return nullptr;
    }
    Node *Add(const std::string &childName)
    {
        children.push_back(std::make_unique<Node>());
        children.back()->name = childName;
        return children.back().get();
    }
    std::unique_ptr<Node> Clone() const
    {
        auto copy = std::make_unique<Node>();
        copy->name = name;
        copy->value = value;
        for (const auto &child : children)
            copy->children.push_back(child->Clone());
        return copy;
    }
};

void Print(const Node &node, std::string &out)
{
    out += '(' + node.name + '=' + (node.value ? '"' + *node.value + '"' : std::string("<none>"));
    for (const auto &child : node.children)
        Print(*child, out);
    out += ')';
}

// Names print as first registered (the codec keeps the first spelling); the model stores the same spelling.
void Print(const ATTRIBUTES &node, std::string &out)
{
    out += std::string("(") + node.GetThisName() + '=' + (node.HasValue() ? '"' + node.GetValue() + '"' : std::string("<none>"));
    for (size_t n = 0; n < node.GetAttributesNum(); n++)
        Print(*node.GetAttributeClass(static_cast<uint32_t>(n)), out);
    out += ')';
}

struct Trees
{
    STRING_CODEC codec;
    ATTRIBUTES real{codec};
    Node model;
    std::mt19937 random{4242};
    std::vector<std::string> pool;

    Trees()
    {
        model.name = "root";
        // More names than the size at which an object gets its hash index: both lookups are exercised.
        for (int n = 0; n < 72; n++)
            pool.push_back(RandomName(random));
    }

    // The spelling the codec keeps for a name: its first registered one.
    std::string Spelling(const std::string &name)
    {
        return codec.Convert(codec.Convert(name.c_str()));
    }
    std::string Name()
    {
        const std::string &name = pool[random() % pool.size()];
        return random() % 3 == 0 ? FlipCase(name, random) : name;
    }
    // A random node of both trees: the same child positions on the way down.
    std::pair<ATTRIBUTES *, Node *> Pick()
    {
        ATTRIBUTES *real_node = &real;
        Node *model_node = &model;
        while (!model_node->children.empty() && random() % 3 != 0)
        {
            const uint32_t n = random() % model_node->children.size();
            real_node = real_node->GetAttributeClass(n);
            model_node = model_node->children[n].get();
        }
        return {real_node, model_node};
    }

    void Step()
    {
        auto [real_node, model_node] = Pick();
        const std::string name = Name();
        const std::string value = "v" + std::to_string(random() % 5);
        switch (random() % 13)
        {
        case 0: // SetAttribute by name: the first child of that name, or a new one
        case 1: {
            real_node->SetAttribute(name, value);
            Node *child = model_node->Child(name);
            if (!child)
                child = model_node->Add(Spelling(name));
            child->value = value;
            break;
        }
        case 2: { // CreateAttribute: always a new child, also under a name that exists
            real_node->CreateAttribute(name, random() % 2 ? value.c_str() : nullptr)->GetThisName();
            Node *child = model_node->Add(Spelling(name));
            if (real_node->GetAttributeClass(static_cast<uint32_t>(model_node->children.size() - 1))->HasValue())
                child->value = value;
            break;
        }
        case 3: { // a path a.b.c created where it is missing
            const std::string second = Name(), third = Name();
            const std::string path = name + "." + second + "." + third;
            ATTRIBUTES *made = real_node->CreateSubAClass(real_node, path.c_str());
            Node *at = model_node;
            for (const std::string &part : {name, second, third})
            {
                Node *child = at->Child(part);
                at = child ? child : at->Add(Spelling(part));
            }
            if (!made || std::string(made->GetThisName()) != at->name)
                Fail("CreateSubAClass " + path);
            break;
        }
        case 4: { // a path looked up
            const std::string second = Name();
            const std::string path = random() % 2 ? name : name + "." + second;
            const ATTRIBUTES *found = real_node->FindAClass(real_node, path.c_str());
            // FindAClass registers the names it is asked for: the model's later spellings follow the codec's.
            const Node *at = model_node->Child(name);
            if (at && path.size() > name.size())
                at = at->Child(second);
            std::string a, b;
            if (found)
                Print(*found, a);
            if (at)
                Print(*at, b);
            if (a != b)
                Fail("FindAClass " + path + ": " + a + " / " + b);
            break;
        }
        case 5: { // values and children by name
            const Node *child = model_node->Child(name);
            const char *text = real_node->GetAttribute(name);
            if ((text != nullptr) != (child && child->value) || (text && *child->value != text))
                Fail("GetAttribute " + name);
            const ATTRIBUTES *found = real_node->GetAttributeClass(name);
            if ((found != nullptr) != (child != nullptr))
                Fail("GetAttributeClass " + name);
            break;
        }
        case 6: { // VerifyAttributeClass: found, or created with an empty value
            real_node->VerifyAttributeClass(name)->GetThisName();
            if (!model_node->Child(name))
                model_node->Add(Spelling(name))->value = "";
            break;
        }
        case 7: { // delete a node below the root, or the root's children
            if (model_node == &model)
            {
                if (random() % 8 == 0)
                {
                    real.DeleteAttributeClassX(&real);
                    model.children.clear();
                }
                break;
            }
            const bool deleted = real.DeleteAttributeClassX(real_node);
            Node *parent = FindParent(model, model_node);
            for (auto it = parent->children.begin(); it != parent->children.end(); ++it)
                if (it->get() == model_node)
                {
                    parent->children.erase(it);
                    break;
                }
            if (!deleted)
                Fail("DeleteAttributeClassX refused a node of the tree");
            break;
        }
        case 8: // rename: the later lookups by name must follow
            if (model_node != &model)
            {
                if (random() % 2)
                    real_node->SetName(name);
                else
                    real_node->SetNameCode(codec.Convert(name.c_str()));
                model_node->name = Spelling(name);
            }
            break;
        case 9: // the script's Sort: by name, byte by byte
            if (model_node->children.size() < 2 || HasEqualNames(*model_node))
                break; // std::sort is not stable: equal names may land in any order
            real_node->SortByName();
            std::sort(model_node->children.begin(), model_node->children.end(),
                      [](const auto &a, const auto &b) { return a->name < b->name; });
            break;
        case 10: { // a copy assigned over another node (script object assignment): value and children, not the name
            auto [from_real, from_model] = Pick();
            if (IsBelow(*from_model, model_node) || IsBelow(*model_node, from_model))
                break;
            *real_node = from_real->Copy();
            auto copy = from_model->Clone();
            model_node->value = copy->value;
            model_node->children = std::move(copy->children);
            break;
        }
        case 11: { // SetAttribute by code, a null value included
            const uint32_t code = codec.Convert(name.c_str());
            const bool null = random() % 3 == 0;
            real_node->SetAttribute(code, null ? nullptr : value.c_str());
            Node *child = model_node->Child(name);
            if (!child)
                child = model_node->Add(Spelling(name));
            child->value = null ? std::nullopt : std::optional<std::string>(value);
            break;
        }
        case 12: { // by code
            const uint32_t code = codec.Convert(name.c_str());
            const ATTRIBUTES *found = real_node->GetAttributeClassByCode(code);
            const Node *child = model_node->Child(name);
            if ((found != nullptr) != (child != nullptr))
                Fail("GetAttributeClassByCode " + name);
            if (random() % 4 == 0)
            {
                real_node->VerifyAttributeClassByCode(code)->GetThisName();
                if (!child)
                    model_node->Add(Spelling(name))->value = "";
            }
            break;
        }
        }
    }

    static bool HasEqualNames(const Node &node)
    {
        for (size_t a = 0; a < node.children.size(); a++)
            for (size_t b = a + 1; b < node.children.size(); b++)
                if (node.children[a]->name == node.children[b]->name)
                    return true;
        return false;
    }
    static bool IsBelow(const Node &top, const Node *node)
    {
        if (&top == node)
            return true;
        for (const auto &child : top.children)
            if (IsBelow(*child, node))
                return true;
        return false;
    }
    static Node *FindParent(Node &top, const Node *node)
    {
        for (const auto &child : top.children)
        {
            if (child.get() == node)
                return &top;
            if (Node *found = FindParent(*child, node))
                return found;
        }
        return nullptr;
    }
};

void CheckTree()
{
    Trees trees;
    for (int step = 0; step < 200000 && failures == 0; step++)
    {
        trees.Step();
        if (step % 16 == 0)
        {
            std::string real, model;
            Print(trees.real, real);
            Print(trees.model, model);
            if (real != model)
                Fail("after step " + std::to_string(step) + ":\n  engine " + real.substr(0, 2000) + "\n  plain  " +
                     model.substr(0, 2000));
        }
        // Keep the tree from growing without bound.
        if (step % 2000 == 1999)
        {
            trees.real.DeleteAttributeClassX(&trees.real);
            trees.model.children.clear();
        }
    }
}
} // namespace

int main()
{
    CheckCodec();
    CheckTree();
    if (failures)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("script VM names: the codec numbers as before, ATTRIBUTES matches the plain tree");
    return 0;
}
