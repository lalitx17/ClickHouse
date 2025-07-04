#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/GatherUtils/GatherUtils.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeNullable.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Common/typeid_cast.h>
#include <IO/WriteHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

/** arraySlice(arr, offset, length) - make slice of array. Offsets and length may be < 0 or Null
  *   - if offset < 0, indexation from right element
  *   - if length < 0, length = len(array) - (positive_index(offset) - 1) + length
  *   indexation:
  *     [ 1,  2,  3,  4,  5,  6]
  *     [-6, -5, -4, -3, -2, -1]
  *   examples:
  *     arraySlice([1, 2, 3, 4, 5, 6], -4, 2) -> [3, 4]
  *     arraySlice([1, 2, 3, 4, 5, 6], 2, -1) -> [2, 3, 4, 5] (6 - (2 - 1) + (-1) = 4)
  *     arraySlice([1, 2, 3, 4, 5, 6], -5, -1) = arraySlice([1, 2, 3, 4, 5, 6], 2, -1) -> [2, 3, 4, 5]
  */
class FunctionArraySlice : public IFunction
{
public:
    static constexpr auto name = "arraySlice";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionArraySlice>(); }

    String getName() const override { return name; }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        const size_t number_of_arguments = arguments.size();

        if (number_of_arguments < 2 || number_of_arguments > 3)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                            "Number of arguments for function {} doesn't match: passed {}, should be 2 or 3",
                            getName(), number_of_arguments);

        if (arguments[0]->onlyNull())
            return arguments[0];

        const auto * array_type = typeid_cast<const DataTypeArray *>(arguments[0].get());
        if (!array_type)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                            "First argument for function {} must be an array but it has type {}.",
                            getName(), arguments[0]->getName());

        for (size_t i = 1; i < number_of_arguments; ++i)
        {
            if (!isInteger(removeNullable(arguments[i])) && !arguments[i]->onlyNull())
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                                "Argument {} for function {} must be integer but it has type {}.",
                                i, getName(), arguments[i]->getName());
        }

        return arguments[0];
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & return_type, size_t input_rows_count) const override
    {
        if (return_type->onlyNull())
            return return_type->createColumnConstWithDefaultValue(input_rows_count);

        auto array_column = arguments[0].column;
        const auto & offset_column = arguments[1].column;
        const auto & length_column = arguments.size() > 2 ? arguments[2].column : nullptr;

        std::unique_ptr<GatherUtils::IArraySource> source;

        size_t size = array_column->size();
        bool is_const = false;

        if (const auto * const_array_column = typeid_cast<const ColumnConst *>(array_column.get()))
        {
            is_const = true;
            array_column = const_array_column->getDataColumnPtr();
        }

        if (const auto * argument_column_array = typeid_cast<const ColumnArray *>(array_column.get()))
            source = GatherUtils::createArraySource(*argument_column_array, is_const, size);
        else
            throw Exception(ErrorCodes::LOGICAL_ERROR, "First arguments for function {} must be array.", getName());

        ColumnArray::MutablePtr sink;

        if (offset_column->onlyNull())
        {
            if (!length_column || length_column->onlyNull())
            {
                return arguments[0].column;
            }
            if (isColumnConst(*length_column))
                sink = GatherUtils::sliceFromLeftConstantOffsetBounded(*source, 0, length_column->getInt(0));
            else
            {
                auto const_offset_column = ColumnConst::create(ColumnInt8::create(1, 1), size);
                sink = GatherUtils::sliceDynamicOffsetBounded(*source, *const_offset_column, *length_column);
            }
        }
        else if (isColumnConst(*offset_column))
        {
            ssize_t offset = offset_column->getUInt(0);

            if (!length_column || length_column->onlyNull())
            {
                if (offset > 0)
                    sink = GatherUtils::sliceFromLeftConstantOffsetUnbounded(*source, static_cast<size_t>(offset - 1));
                else
                    sink = GatherUtils::sliceFromRightConstantOffsetUnbounded(*source, -static_cast<size_t>(offset));
            }
            else if (isColumnConst(*length_column))
            {
                ssize_t length = length_column->getInt(0);
                if (offset > 0)
                    sink = GatherUtils::sliceFromLeftConstantOffsetBounded(*source, static_cast<size_t>(offset - 1), length);
                else
                    sink = GatherUtils::sliceFromRightConstantOffsetBounded(*source, -static_cast<size_t>(offset), length);
            }
            else
                sink = GatherUtils::sliceDynamicOffsetBounded(*source, *offset_column, *length_column);
        }
        else
        {
            if (!length_column || length_column->onlyNull())
                sink = GatherUtils::sliceDynamicOffsetUnbounded(*source, *offset_column);
            else
                sink = GatherUtils::sliceDynamicOffsetBounded(*source, *offset_column, *length_column);
        }

        return sink;
    }

    bool useDefaultImplementationForConstants() const override { return true; }
    bool useDefaultImplementationForNulls() const override { return false; }
};


REGISTER_FUNCTION(ArraySlice)
{
    FunctionDocumentation::Description description = "Returns a slice of the array, with `NULL` elements included.";
    FunctionDocumentation::Syntax syntax = "arraySlice(arr, offset [, length])";
    FunctionDocumentation::Arguments arguments = {
        {"arr", "Array to slice.", {"Array(T)"}},
        {"offset", "Indent from the edge of the array. A positive value indicates an offset on the left, and a negative value is an indent on the right. Numbering of the array items begins with `1`.", {"(U)Int*"}},
        {"length", "The length of the required slice. If you specify a negative value, the function returns an open slice `[offset, array_length - length]`. If you omit the value, the function returns the slice `[offset, the_end_of_array]`.", {"(U)Int*"}},
    };
    FunctionDocumentation::ReturnedValue returned_value = {"Returns a slice of the array with `length` elements from the specified `offset`", {"Array(T)"}};
    FunctionDocumentation::Examples examples = {{"Usage example", "SELECT arraySlice([1, 2, NULL, 4, 5], 2, 3) AS res;", "[2, NULL, 4]"}};
    FunctionDocumentation::IntroducedIn introduced_in = {1, 1};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Array;
    FunctionDocumentation documentation = {description, syntax, arguments, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionArraySlice>(documentation);
}


}
