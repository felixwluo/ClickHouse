#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeFactory.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>


namespace DB
{

namespace
{

class FunctionEmptyArray : public IFunction
{
private:
    String element_type;

public:
    static String getNameImpl(const String & element_type) { return "emptyArray" + element_type; }

    explicit FunctionEmptyArray(const String & element_type_) : element_type(element_type_) {}

private:
    String getName() const override
    {
        return getNameImpl(element_type);
    }

    size_t getNumberOfArguments() const override { return 0; }

    DataTypePtr getReturnTypeImpl(const DataTypes & /*arguments*/) const override
    {
        return std::make_shared<DataTypeArray>(DataTypeFactory::instance().get(element_type));
    }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName &, const DataTypePtr &, size_t input_rows_count) const override
    {
        return ColumnArray::create(
            DataTypeFactory::instance().get(element_type)->createColumn(),
            ColumnArray::ColumnOffsets::create(input_rows_count, 0));
    }
};

void registerFunction(FunctionFactory & factory, const String & element_type)
{
    FunctionDocumentation::Description description = fmt::format("Returns an empty {} array", element_type);
    FunctionDocumentation::Syntax syntax = fmt::format("emptyArray{}()", element_type);
    FunctionDocumentation::Arguments arguments = {};
    FunctionDocumentation::ReturnedValue returned_value = {fmt::format("An empty {} array.", element_type), {"Array(T)"}};
    FunctionDocumentation::Examples examples = {{"Usage example", fmt::format("SELECT emptyArray{}", element_type), "[]"}};
    FunctionDocumentation::IntroducedIn introduced_in = {1, 1};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Array;
    FunctionDocumentation documentation = {description, syntax, arguments, returned_value, examples, introduced_in, category};

    factory.registerFunction(
        FunctionEmptyArray::getNameImpl(element_type),
        [element_type](ContextPtr){ return std::make_shared<FunctionEmptyArray>(element_type); },
        documentation
    );
}

}

REGISTER_FUNCTION(EmptyArray)
{
    registerFunction(factory, "UInt8");
    registerFunction(factory, "UInt16");
    registerFunction(factory, "UInt32");
    registerFunction(factory, "UInt64");
    registerFunction(factory, "Int8");
    registerFunction(factory, "Int16");
    registerFunction(factory, "Int32");
    registerFunction(factory, "Int64");
    registerFunction(factory, "Float32");
    registerFunction(factory, "Float64");
    registerFunction(factory, "Date");
    registerFunction(factory, "DateTime");
    registerFunction(factory, "String");
}

}
