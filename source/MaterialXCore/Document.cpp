//
// TM & (c) 2017 Lucasfilm Entertainment Company Ltd. and Lucasfilm Ltd.
// All rights reserved.  See LICENSE.txt for license.
//

#include <MaterialXCore/Document.h>

#include <MaterialXCore/Util.h>

#include <mutex>

namespace MaterialX
{

const string Document::CMS_ATTRIBUTE = "cms";
const string Document::CMS_CONFIG_ATTRIBUTE = "cmsconfig";

namespace {

const string DOCUMENT_VERSION_STRING = std::to_string(MATERIALX_MAJOR_VERSION) + "." +
                                       std::to_string(MATERIALX_MINOR_VERSION);

template<class T> shared_ptr<T> updateChildSubclass(ElementPtr parent, ElementPtr origChild)
{
    string childName = origChild->getName();
    int childIndex = parent->getChildIndex(childName);
    parent->removeChild(childName);
    shared_ptr<T> newChild = parent->addChild<T>(childName);
    parent->setChildIndex(childName, childIndex);
    newChild->copyContentFrom(origChild);
    return newChild;
}

bool convertMaterialsToNodes(DocumentPtr doc)
{
    bool modified = false;

    vector<MaterialPtr> materials = doc->getMaterials();
    for (auto m : materials)
    {
        // See if a node of this name has already been created.
        // Should not occur otherwise there are duplicate existing
        // Material elements.
        string materialName = m->getName();
        if (doc->getNode(materialName))
        {
            throw Exception("Material node already exists: " + materialName);
        }

        // Create a temporary name for the material element 
        // so the new node can reuse the existing name.
        string validName = doc->createValidChildName(materialName + "1");
        m->setName(validName);

        // Create a new material node
        NodePtr materialNode = nullptr;

        ShaderRefPtr sr;
        // Only include the shader refs explicitly specified on the material instance
        vector<ShaderRefPtr> srs = m->getShaderRefs();
        for (size_t i = 0; i < srs.size(); i++)
        {
            sr = srs[i];

            // See if shader has been created already.
            // Should not occur as the shaderref is a uniquely named
            // child of a uniquely named material element, but the two combined
            // may have been used for another node instance which not a shader node.
            string shaderNodeName = materialName + "_" + sr->getName();
            NodePtr existingShaderNode = doc->getNode(shaderNodeName);
            if (existingShaderNode)
            {
                const string& existingType = existingShaderNode->getType();
                if (existingType == VOLUME_SHADER_TYPE_STRING ||
                    existingType == SURFACE_SHADER_TYPE_STRING ||
                    existingType == DISPLACEMENT_SHADER_TYPE_STRING)
                {
                    throw Exception("Shader node already exists: " + shaderNodeName);
                }
                else
                {
                    shaderNodeName = doc->createValidChildName(shaderNodeName);
                }
            }

            modified = true;

            // Find the shader type if defined
            string shaderNodeType = SURFACE_SHADER_TYPE_STRING;
            NodeDefPtr nodeDef = sr->getNodeDef();
            if (nodeDef)
            {
                shaderNodeType = nodeDef->getType();
            }

            // Add in a new shader node
            const string shaderNodeCategory = sr->getNodeString();
            NodePtr shaderNode = doc->addNode(shaderNodeCategory, shaderNodeName, shaderNodeType);
            shaderNode->setSourceUri(sr->getSourceUri());

            for (auto valueElement : sr->getChildrenOfType<ValueElement>())
            {
                ElementPtr portChild = nullptr;

                // Copy over bindinputs as inputs, and bindparams as params
                if (valueElement->isA<BindInput>())
                {
                    portChild = shaderNode->addInput(valueElement->getName(), valueElement->getType());
                }
                else if (valueElement->isA<BindParam>())
                {
                    portChild = shaderNode->addParameter(valueElement->getName(), valueElement->getType());
                }
                if (portChild)
                {
                    // Copy over attributes.
                    // Note: We preserve inputs which have nodegraph connections,
                    // as well as top level output connections.
                    portChild->copyContentFrom(valueElement);
                }
            }

            // Copy over any bindtokens as tokens
            for (auto bt : sr->getBindTokens())
            {
                TokenPtr token = shaderNode->addToken(bt->getName());
                token->copyContentFrom(bt);
            }

            // Create a new material node if not already created and
            // add a reference from the material node to the new shader node
            if (!materialNode)
            {
                // Set the type of material based on current assumption that
                // surfaceshaders + displacementshaders result in a surfacematerial
                // while a volumeshader means a volumematerial needs to be created.
                string materialNodeCategory =
                    (shaderNodeType != VOLUME_SHADER_TYPE_STRING) ? SURFACE_MATERIAL_NODE_STRING
                    : VOLUME_MATERIAL_NODE_STRING;
                materialNode = doc->addNode(materialNodeCategory, materialName, MATERIAL_TYPE_STRING);
                materialNode->setSourceUri(m->getSourceUri());
                // Note: Inheritance does not get transfered to the node we do
                // not perform the following:
                //      - materialNode->setInheritString(m->getInheritString()); 
            }
            // Create input to replace each shaderref. Use shaderref name as unique
            // input name.
            InputPtr shaderInput = materialNode->addInput(shaderNodeType, shaderNodeType);
            shaderInput->setNodeName(shaderNode->getName());
            // Make sure to copy over any target and version information from the shaderref.
            if (!sr->getTarget().empty())
            {
                shaderInput->setTarget(sr->getTarget());
            }
            if (!sr->getVersionString().empty())
            {
                shaderInput->setVersionString(sr->getVersionString());
            }
        }

        // Remove existing material element
        doc->removeChild(m->getName());
    }
    return modified;
}

} // anonymous namespace

//
// Document factory function
//

DocumentPtr createDocument()
{
    return Document::createDocument<Document>();
}

//
// Document cache
//

class Document::Cache
{
  public:
    Cache() :
        valid(false)
    {
    }
    ~Cache() { }

    void refresh()
    {
        // Thread synchronization for multiple concurrent readers of a single document.
        std::lock_guard<std::mutex> guard(mutex);

        if (!valid)
        {
            // Clear the existing cache.
            portElementMap.clear();
            nodeDefMap.clear();
            implementationMap.clear();

            // Traverse the document to build a new cache.
            for (ElementPtr elem : doc.lock()->traverseTree())
            {
                const string& nodeName = elem->getAttribute(PortElement::NODE_NAME_ATTRIBUTE);
                const string& nodeString = elem->getAttribute(NodeDef::NODE_ATTRIBUTE);
                const string& nodeDefString = elem->getAttribute(InterfaceElement::NODE_DEF_ATTRIBUTE);

                if (!nodeName.empty())
                {
                    PortElementPtr portElem = elem->asA<PortElement>();
                    if (portElem)
                    {
                        portElementMap.insert(std::pair<string, PortElementPtr>(
                            portElem->getQualifiedName(nodeName),
                            portElem));
                    }
                }
                if (!nodeString.empty())
                {
                    NodeDefPtr nodeDef = elem->asA<NodeDef>();
                    if (nodeDef)
                    {
                        nodeDefMap.insert(std::pair<string, NodeDefPtr>(
                            nodeDef->getQualifiedName(nodeString),
                            nodeDef));
                    }
                }
                if (!nodeDefString.empty())
                {
                    InterfaceElementPtr interface = elem->asA<InterfaceElement>();
                    if (interface && (interface->isA<Implementation>() || interface->isA<NodeGraph>()))
                    {
                        implementationMap.insert(std::pair<string, InterfaceElementPtr>(
                            interface->getQualifiedName(nodeDefString),
                            interface));
                    }
                }
            }

            valid = true;
        }
    }

  public:
    weak_ptr<Document> doc;
    std::mutex mutex;
    bool valid;
    std::unordered_multimap<string, PortElementPtr> portElementMap;
    std::unordered_multimap<string, NodeDefPtr> nodeDefMap;
    std::unordered_multimap<string, InterfaceElementPtr> implementationMap;
};

//
// Document methods
//

Document::Document(ElementPtr parent, const string& name) :
    GraphElement(parent, CATEGORY, name),
    _cache(std::unique_ptr<Cache>(new Cache))
{
}

Document::~Document()
{
}

void Document::initialize()
{
    _root = getSelf();

    DocumentPtr doc = getDocument();
    _cache->doc = doc;

    clearContent();
    setVersionString(DOCUMENT_VERSION_STRING);
}

void Document::importLibrary(const ConstDocumentPtr& library)
{
    if (!library)
    {
        return;
    }

    for (const ConstElementPtr& child : library->getChildren())
    {
        string childName = child->getQualifiedName(child->getName());

        // Check for duplicate elements.
        ConstElementPtr previous = getChild(childName);
        if (previous)
        {
            continue;
        }

        // Create the imported element.
        ElementPtr childCopy = addChildOfCategory(child->getCategory(), childName, !previous);
        childCopy->copyContentFrom(child);
        if (!childCopy->hasFilePrefix() && library->hasFilePrefix())
        {
            childCopy->setFilePrefix(library->getFilePrefix());
        }
        if (!childCopy->hasGeomPrefix() && library->hasGeomPrefix())
        {
            childCopy->setGeomPrefix(library->getGeomPrefix());
        }
        if (!childCopy->hasColorSpace() && library->hasColorSpace())
        {
            childCopy->setColorSpace(library->getColorSpace());
        }
        if (!childCopy->hasNamespace() && library->hasNamespace())
        {
            childCopy->setNamespace(library->getNamespace());
        }
        if (!childCopy->hasSourceUri() && library->hasSourceUri())
        {
            childCopy->setSourceUri(library->getSourceUri());
        }
    }
}

StringSet Document::getReferencedSourceUris() const
{
    StringSet sourceUris;
    for (ElementPtr elem : traverseTree())
    {
        if (elem->hasSourceUri())
        {
            sourceUris.insert(elem->getSourceUri());
        }
    }
    return sourceUris;
}

std::pair<int, int> Document::getVersionIntegers() const
{
    if (!hasVersionString())
    {
        return {MATERIALX_MAJOR_VERSION, MATERIALX_MINOR_VERSION};
    }
    return Element::getVersionIntegers();
}

vector<PortElementPtr> Document::getMatchingPorts(const string& nodeName) const
{
    // Refresh the cache.
    _cache->refresh();

    // Find all port elements matching the given node name.
    vector<PortElementPtr> ports;
    auto keyRange = _cache->portElementMap.equal_range(nodeName);
    for (auto it = keyRange.first; it != keyRange.second; ++it)
    {
        ports.push_back(it->second);
    }

    // Return the matches.
    return ports;
}

ValuePtr Document::getGeomPropValue(const string& geomPropName, const string& geom) const
{
    ValuePtr value;
    for (GeomInfoPtr geomInfo : getGeomInfos())
    {
        if (!geomStringsMatch(geom, geomInfo->getActiveGeom()))
        {
            continue;
        }
        GeomPropPtr geomProp = geomInfo->getGeomProp(geomPropName);
        if (geomProp)
        {
            value = geomProp->getValue();
        }
    }
    return value;
}

vector<NodeDefPtr> Document::getMatchingNodeDefs(const string& nodeName) const
{
    // Refresh the cache.
    _cache->refresh();

    // Find all nodedefs matching the given node name.
    vector<NodeDefPtr> nodeDefs;
    auto keyRange = _cache->nodeDefMap.equal_range(nodeName);
    for (auto it = keyRange.first; it != keyRange.second; ++it)
    {
        nodeDefs.push_back(it->second);
    }

    // Return the matches.
    return nodeDefs;
}

vector<InterfaceElementPtr> Document::getMatchingImplementations(const string& nodeDef) const
{
    // Refresh the cache.
    _cache->refresh();

    // Find all implementations matching the given nodedef string.
    vector<InterfaceElementPtr> implementations;
    auto keyRange = _cache->implementationMap.equal_range(nodeDef);
    for (auto it = keyRange.first; it != keyRange.second; ++it)
    {
        implementations.push_back(it->second);
    }

    // Return the matches.
    return implementations;
}

bool Document::validate(string* message) const
{
    bool res = true;
    validateRequire(hasVersionString(), res, message, "Missing version string");
    return GraphElement::validate(message) && res;
}

void Document::upgradeVersion(bool applyFutureUpdates)
{
    std::pair<int, int> versions = getVersionIntegers();
    int majorVersion = versions.first;
    int minorVersion = versions.second;
    if (majorVersion == MATERIALX_MAJOR_VERSION &&
        minorVersion == MATERIALX_MINOR_VERSION &&
        !applyFutureUpdates)
    {
        return;
    }

    // Upgrade from v1.22 to v1.23
    if (majorVersion == 1 && minorVersion == 22)
    {
        for (ElementPtr elem : traverseTree())
        {
            if (elem->isA<TypedElement>())
            {
                TypedElementPtr typedElem = elem->asA<TypedElement>();
                if (typedElem->getType() == "vector")
                {
                    typedElem->setType(getTypeString<Vector3>());
                }
            }
        }
        minorVersion = 23;
    }

    // Upgrade from v1.23 to v1.24
    if (majorVersion == 1 && minorVersion == 23)
    {
        for (ElementPtr elem : traverseTree())
        {
            if (elem->getCategory() == "shader" && elem->hasAttribute("shadername"))
            {
                elem->setAttribute(NodeDef::NODE_ATTRIBUTE, elem->getAttribute("shadername"));
                elem->removeAttribute("shadername");
            }
            vector<ElementPtr> origChildren = elem->getChildren();
            for (ElementPtr child : origChildren)
            {
                if (child->getCategory() == "assign")
                {
                    updateChildSubclass<MaterialAssign>(elem, child);
                }
            }
        }
        minorVersion = 24;
    }

    // Upgrade from v1.24 to v1.25
    if (majorVersion == 1 && minorVersion == 24)
    {
        for (ElementPtr elem : traverseTree())
        {
            if (elem->isA<Input>() && elem->hasAttribute("graphname"))
            {
                elem->setAttribute("opgraph", elem->getAttribute("graphname"));
                elem->removeAttribute("graphname");
            }
        }
        minorVersion = 25;
    }

    // Upgrade from v1.25 to v1.26
    if (majorVersion == 1 && minorVersion == 25)
    {
        for (ElementPtr elem : traverseTree())
        {
            if (elem->getCategory() == "constant")
            {
                ElementPtr param = elem->getChild("color");
                if (param)
                {
                    param->setName("value");
                }
            }
        }
        minorVersion = 26;
    }

    // Upgrade from v1.26 to v1.34
    if (majorVersion == 1 && minorVersion == 26)
    {
        // Upgrade elements in place.
        for (ElementPtr elem : traverseTree())
        {
            vector<ElementPtr> origChildren = elem->getChildren();
            for (ElementPtr child : origChildren)
            {
                if (child->getCategory() == "opgraph")
                {
                    updateChildSubclass<NodeGraph>(elem, child);
                }
                else if (child->getCategory() == "shader")
                {
                    NodeDefPtr nodeDef = updateChildSubclass<NodeDef>(elem, child);
                    if (nodeDef->hasAttribute("shadertype"))
                    {
                        nodeDef->setType(SURFACE_SHADER_TYPE_STRING);
                        nodeDef->removeAttribute("shadertype");
                    }
                    if (nodeDef->hasAttribute("shaderprogram"))
                    {
                        nodeDef->setNodeString(nodeDef->getAttribute("shaderprogram"));
                        nodeDef->removeAttribute("shaderprogram");
                    }
                }
                else if (child->isA<Parameter>())
                {
                    ParameterPtr param = child->asA<Parameter>();
                    if (param->getType() == "opgraphnode")
                    {
                        if (elem->isA<Node>())
                        {
                            InputPtr input = updateChildSubclass<Input>(elem, param);
                            input->setNodeName(input->getAttribute("value"));
                            input->removeAttribute("value");
                            if (input->getConnectedNode())
                            {
                                input->setType(input->getConnectedNode()->getType());
                            }
                            else
                            {
                                input->setType(getTypeString<Color3>());
                            }
                        }
                        else if (elem->isA<Output>())
                        {
                            if (child->getName() == "in")
                            {
                                elem->setAttribute("nodename", child->getAttribute("value"));
                            }
                            elem->removeChild(child->getName());
                        }
                    }
               }
            }
        }

        // Assign nodedef names to shaderrefs.
        for (MaterialPtr mat : getMaterials())
        {
            for (ShaderRefPtr shaderRef : mat->getShaderRefs())
            {
                if (!shaderRef->getNodeDef())
                {
                    NodeDefPtr nodeDef = getNodeDef(shaderRef->getName());
                    if (nodeDef)
                    {
                        shaderRef->setNodeDefString(nodeDef->getName());
                    }
                }
            }
        }

        // Move connections from nodedef inputs to bindinputs.
        for (NodeDefPtr nodeDef : getNodeDefs())
        {
            for (InputPtr input : nodeDef->getActiveInputs())
            {
                if (input->hasAttribute("opgraph") && input->hasAttribute("graphoutput"))
                {
                    for (MaterialPtr mat : getMaterials())
                    {
                        for (ShaderRefPtr shaderRef : mat->getShaderRefs())
                        {
                            if (shaderRef->getNodeDef() == nodeDef && !shaderRef->getChild(input->getName()))
                            {
                                BindInputPtr bind = shaderRef->addBindInput(input->getName(), input->getType());
                                bind->setNodeGraphString(input->getAttribute("opgraph"));
                                bind->setOutputString(input->getAttribute("graphoutput"));
                            }
                        }
                    }
                    input->removeAttribute("opgraph");
                    input->removeAttribute("graphoutput");
                }
            }
        }

        // Combine udim assignments into udim sets.
        for (GeomInfoPtr geomInfo : getGeomInfos())
        {
            vector<ElementPtr> origChildren = geomInfo->getChildren();
            for (ElementPtr child : origChildren)
            {
                if (child->getCategory() == "geomattr")
                {
                    updateChildSubclass<GeomProp>(geomInfo, child);
                }
            }
        }
        if (getGeomPropValue("udim") && !getGeomPropValue("udimset"))
        {
            StringSet udimSet;
            for (GeomInfoPtr geomInfo : getGeomInfos())
            {
                for (GeomPropPtr geomProp : geomInfo->getGeomProps())
                {
                    if (geomProp->getName() == "udim")
                    {
                        udimSet.insert(geomProp->getValueString());
                    }
                }
            }

            std::string udimSetString;
            for (const std::string& udim : udimSet)
            {
                if (udimSetString.empty())
                {
                    udimSetString = udim;
                }
                else
                {
                    udimSetString += ", " + udim;
                }
            }

            GeomInfoPtr udimSetInfo = addGeomInfo();
            udimSetInfo->setGeomPropValue("udimset", udimSetString, getTypeString<StringVec>());
        }

        minorVersion = 34;
    }

    // Upgrade from v1.34 to v1.35
    if (majorVersion == 1 && minorVersion == 34)
    {
        for (ElementPtr elem : traverseTree())
        {
            TypedElementPtr typedElem = elem->asA<TypedElement>();
            ValueElementPtr valueElem = elem->asA<ValueElement>();
            MaterialAssignPtr matAssign = elem->asA<MaterialAssign>();
            if (typedElem && typedElem->getType() == "matrix")
            {
                typedElem->setType(getTypeString<Matrix44>());
            }
            if (valueElem && valueElem->hasAttribute("default"))
            {
                valueElem->setValueString(elem->getAttribute("default"));
                valueElem->removeAttribute("default");
            }
            if (matAssign)
            {
                matAssign->setMaterial(matAssign->getName());
            }
        }
        minorVersion = 35;
    }

    // Upgrade from v1.35 to v1.36
    if (majorVersion == 1 && minorVersion == 35)
    {
        for (ElementPtr elem : traverseTree())
        {
            ValueElementPtr valueElem = elem->asA<ValueElement>();
            MaterialPtr material = elem->asA<Material>();
            LookPtr look = elem->asA<Look>();
            GeomInfoPtr geomInfo = elem->asA<GeomInfo>();

            if (valueElem)
            {
                if (valueElem->getType() == GEOMNAME_TYPE_STRING &&
                    valueElem->getValueString() == "*")
                {
                    valueElem->setValueString(UNIVERSAL_GEOM_NAME);
                }
                if (valueElem->getType() == FILENAME_TYPE_STRING)
                {
                    StringMap stringMap;
                    stringMap["%UDIM"] = UDIM_TOKEN;
                    stringMap["%UVTILE"] = UV_TILE_TOKEN;
                    valueElem->setValueString(replaceSubstrings(valueElem->getValueString(), stringMap));
                }
            }

            vector<ElementPtr> origChildren = elem->getChildren();
            for (ElementPtr child : origChildren)
            {
                if (material && child->getCategory() == "override")
                {
                    for (ShaderRefPtr shaderRef : material->getShaderRefs())
                    {
                        NodeDefPtr nodeDef = shaderRef->getNodeDef();
                        if (nodeDef)
                        {
                            for (ValueElementPtr activeValue : nodeDef->getActiveValueElements())
                            {
                                if (activeValue->getAttribute("publicname") == child->getName() &&
                                    !shaderRef->getChild(child->getName()))
                                {
                                    if (activeValue->isA<Parameter>())
                                    {
                                        BindParamPtr bindParam = shaderRef->addBindParam(activeValue->getName(), activeValue->getType());
                                        bindParam->setValueString(child->getAttribute("value"));
                                    }
                                    else if (activeValue->isA<Input>())
                                    {
                                        BindInputPtr bindInput = shaderRef->addBindInput(activeValue->getName(), activeValue->getType());
                                        bindInput->setValueString(child->getAttribute("value"));
                                    }
                                }
                            }
                        }
                    }
                    elem->removeChild(child->getName());
                }
                else if (material && child->getCategory() == "materialinherit")
                {
                    elem->setInheritString(child->getAttribute("material"));
                    elem->removeChild(child->getName());
                }
                else if (look && child->getCategory() == "lookinherit")
                {
                    elem->setInheritString(child->getAttribute("look"));
                    elem->removeChild(child->getName());
                }
            }
        }
        minorVersion = 36;
    }

    // Upgrade from 1.36 to 1.37
    if (majorVersion == 1 && minorVersion == 36)
    {
        // Convert type attributes to child outputs.
        for (NodeDefPtr nodeDef : getNodeDefs())
        {
            InterfaceElementPtr interfaceElem = std::static_pointer_cast<InterfaceElement>(nodeDef);
            if (interfaceElem && interfaceElem->hasType())
            {
                string type = interfaceElem->getAttribute(TypedElement::TYPE_ATTRIBUTE);
                OutputPtr outputPtr;
                if (!type.empty() && type != MULTI_OUTPUT_TYPE_STRING)
                {
                    outputPtr = interfaceElem->getOutput("out");
                    if (!outputPtr)
                    {
                        outputPtr = interfaceElem->addOutput("out", type);
                    }
                }
                interfaceElem->removeAttribute(TypedElement::TYPE_ATTRIBUTE);

                const string& defaultInput = interfaceElem->getAttribute(Output::DEFAULT_INPUT_ATTRIBUTE);
                if (outputPtr && !defaultInput.empty())
                {
                    outputPtr->setAttribute(Output::DEFAULT_INPUT_ATTRIBUTE, defaultInput);
                }
                interfaceElem->removeAttribute(Output::DEFAULT_INPUT_ATTRIBUTE);
            }
        }

        // Convert geometric attributes to geometric properties.
        for (GeomInfoPtr geomInfo : getGeomInfos())
        {
            vector<ElementPtr> origChildren = geomInfo->getChildren();
            for (ElementPtr child : origChildren)
            {
                if (child->getCategory() == "geomattr")
                {
                    updateChildSubclass<GeomProp>(geomInfo, child);
                }
            }
        }
        for (ElementPtr elem : traverseTree())
        {
            NodePtr node = elem->asA<Node>();
            if (!node)
            {
                continue;
            }

            if (node->getCategory() == "geomattrvalue")
            {
                node->setCategory("geompropvalue");
                if (node->hasAttribute("attrname"))
                {
                    node->setAttribute("geomprop", node->getAttribute("attrname"));
                    node->removeAttribute("attrname");
                }
            }
        }

        for (ElementPtr elem : traverseTree())
        {
            NodePtr node = elem->asA<Node>();
            if (!node)
            {
                continue;
            }
            const string& nodeCategory = node->getCategory();

            // Change category from "invert to "invertmatrix" for matrix invert nodes
            if (nodeCategory == "invert" &&
                (node->getType() == getTypeString<Matrix33>() || node->getType() == getTypeString<Matrix44>()))
            {
                node->setCategory("invertmatrix");
            }

            // Change category from "rotate" to "rotate2d" or "rotate3d" nodes
            else if (nodeCategory == "rotate")
            {
                node->setCategory((node->getType() == getTypeString<Vector2>()) ? "rotate2d" : "rotate3d");
            }

            // Convert "compare" node to "ifgreatereq".
            else if (nodeCategory == "compare")
            {
                node->setCategory("ifgreatereq");
                InputPtr intest = node->getInput("intest");
                if (intest)
                {
                    intest->setName("value1");
                }
                ParameterPtr cutoff = node->getParameter("cutoff");
                if (cutoff)
                {
                    InputPtr value2 = node->addInput("value2");
                    value2->copyContentFrom(cutoff);
                    node->removeChild(cutoff->getName());
                }
                InputPtr in1 = node->getInput("in1");
                InputPtr in2 = node->getInput("in2");
                if (in1 && in2)
                {
                    in1->setName(createValidChildName("temp"));
                    in2->setName("in1");
                    in1->setName("in2");
                }
            }

            // Change nodes with category "tranform[vector|point|normal]",
            // which are not fromspace/tospace variants, to "transformmatrix"
            else if (nodeCategory == "transformpoint" ||
                     nodeCategory == "transformvector" ||
                     nodeCategory == "transformnormal")
            {
                if (!node->getChild("fromspace") && !node->getChild("tospace"))
                {
                    node->setCategory("transformmatrix");
                }
            }

            // Convert "combine" to "combine2", "combine3" or "combine4"
            else if (nodeCategory == "combine")
            {
                if (node->getChild("in4"))
                {
                    node->setCategory("combine4");
                }
                else if (node->getChild("in3"))
                {
                    node->setCategory("combine3");
                }
                else
                {
                    node->setCategory("combine2");
                }
            }

            // Convert "separate" to "separate2", "separate3" or "separate4"
            else if (nodeCategory == "separate")
            {
                InputPtr in = node->getInput("in");
                if (in)
                {
                    const string& inType = in->getType();
                    if (inType == getTypeString<Vector4>() || inType == getTypeString<Color4>())
                    {
                        node->setCategory("separate4");
                    }
                    else if (inType == getTypeString<Vector3>() || inType == getTypeString<Color3>())
                    {
                        node->setCategory("separate3");
                    }
                    else
                    {
                        node->setCategory("separate2");
                    }
                }
            }

            // Convert backdrop nodes to backdrop elements
            else if (nodeCategory == "backdrop")
            {
                const string& nodeName = node->getName();
                BackdropPtr backdrop = addBackdrop(nodeName);
                for (const ParameterPtr param : node->getParameters())
                {
                    ValuePtr value = param ? param->getValue() : nullptr;
                    if (value)
                    {
                        if (value->isA<string>())
                        {
                            backdrop->setAttribute(param->getName(), value->asA<string>());
                        }
                        else if (value->isA<float>())
                        {
                            backdrop->setTypedAttribute(param->getName(), value->asA<float>());
                        }
                    }
                }
                removeNode(nodeName);
            }
        }

        // Remove deprecated nodedefs
        removeNodeDef("ND_backdrop");
        removeNodeDef("ND_invert_matrix33");
        removeNodeDef("ND_invert_matrix44");
        removeNodeDef("ND_rotate_vector2");
        removeNodeDef("ND_rotate_vector3");

        minorVersion = 37;
    }

    if (majorVersion == MATERIALX_MAJOR_VERSION &&
        minorVersion == MATERIALX_MINOR_VERSION)
    {
        if (applyFutureUpdates)
        {
            convertMaterialsToNodes(getDocument());
        }
        setVersionString(DOCUMENT_VERSION_STRING);
    }
}

void Document::onAddElement(ElementPtr, ElementPtr)
{
    _cache->valid = false;
}

void Document::onRemoveElement(ElementPtr, ElementPtr)
{
    _cache->valid = false;
}

void Document::onSetAttribute(ElementPtr, const string&, const string&)
{
    _cache->valid = false;
}

void Document::onRemoveAttribute(ElementPtr, const string&)
{
    _cache->valid = false;
}

void Document::onCopyContent(ElementPtr)
{
    _cache->valid = false;
}

void Document::onClearContent(ElementPtr)
{
    _cache->valid = false;
}

} // namespace MaterialX
