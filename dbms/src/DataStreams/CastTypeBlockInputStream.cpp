#include <DataStreams/CastTypeBlockInputStream.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeString.h>
#include <Interpreters/ExpressionActions.h>
#include <Functions/FunctionFactory.h>
#include <Functions/IFunction.h>


namespace DB
{


CastTypeBlockInputStream::CastTypeBlockInputStream(
    const Context & context_,
    BlockInputStreamPtr input_,
    const Block & ref_defenition_)
    : context(context_), ref_defenition(ref_defenition_)
{
    children.push_back(input_);
}

String CastTypeBlockInputStream::getName() const
{
    return "CastType";
}

String CastTypeBlockInputStream::getID() const
{
    std::stringstream res;
    res << "CastType(" << children.back()->getID() << ")";
    return res.str();
}

Block CastTypeBlockInputStream::readImpl()
{
    Block block = children.back()->read();

    if (!block)
        return block;

    if (!initialized)
    {
        initialized = true;
        collectDifferent(block, ref_defenition);
    }

    if (cast_description.empty())
        return block;

    size_t num_columns = block.columns();
    Block res;

    for (size_t col = 0; col < num_columns; ++col)
    {
        const auto & src_column = block.getByPosition(col);
        auto it = cast_description.find(col);

        if (it == cast_description.end())
        {
            res.insert(src_column);
        }
        else
        {
            CastElement & cast_element = it->second;

            size_t tmp_col = cast_element.tmp_col_offset;
            ColumnNumbers arguments{tmp_col, tmp_col + 1};
            tmp_conversion_block.getByPosition(tmp_col).column = src_column.column;

            cast_element.function->execute(tmp_conversion_block, arguments, tmp_col + 2);
            res.insert(tmp_conversion_block.getByPosition(tmp_col + 2));
        }
    }

    return res;
}


CastTypeBlockInputStream::CastElement::CastElement(std::shared_ptr<IFunction> && function_, size_t tmp_col_offset_)
    : function(std::move(function_)), tmp_col_offset(tmp_col_offset_) {}


void CastTypeBlockInputStream::collectDifferent(const Block & src_block, const Block & ref_sample)
{
    for (size_t src_col = 0; src_col < src_block.columns(); ++src_col)
    {
        const auto & src_column = src_block.getByPosition(src_col);

        /// Skip, if it is a problem, it will be detected on the next pipeline stage
        if (!ref_sample.has(src_column.name))
            continue;

        const auto & ref_column = ref_sample.getByName(src_column.name);

        /// Force conversion if source and destination types is different.
        if (!ref_column.type->equals(*src_column.type))
        {
            ColumnWithTypeAndName src_columnn_copy = src_column.cloneEmpty();
            ColumnWithTypeAndName alias_column(std::make_shared<ColumnConstString>(1, ref_column.type->getName()), std::make_shared<DataTypeString>(), "");
            ColumnWithTypeAndName result_column(nullptr, ref_column.type->clone(), src_column.name);

            DataTypePtr unused_return_type;
            std::vector<ExpressionAction> unused_prerequisites;
            ColumnsWithTypeAndName arguments{src_columnn_copy, alias_column};

            /// Prepares function to execution. TODO It is not obvious.
            auto cast_function = FunctionFactory::instance().get("CAST", context);
            cast_function->getReturnTypeAndPrerequisites(arguments, unused_return_type, unused_prerequisites);

            tmp_conversion_block.insert(src_column);
            tmp_conversion_block.insert(alias_column);
            tmp_conversion_block.insert(result_column);

            size_t tmp_col_offset = cast_description.size() * 3;
            cast_description.emplace(src_col, CastElement(std::move(cast_function), tmp_col_offset));
        }
    }
}

}
