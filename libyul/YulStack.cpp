/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <libyul/YulStack.h>

#include <libyul/AsmAnalysis.h>
#include <libyul/AsmAnalysisInfo.h>
#include <libyul/backends/evm/EthAssemblyAdapter.h>
#include <libyul/backends/evm/EVMCodeTransform.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libyul/backends/evm/EVMObjectCompiler.h>
#include <libyul/backends/evm/EVMMetrics.h>
#include <libyul/ObjectParser.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/Suite.h>
#include <libevmasm/Assembly.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/SourceReferenceFormatter.h>
#include <libsolidity/interface/OptimiserSettings.h>

#include <boost/algorithm/string.hpp>

#include <optional>

using namespace solidity;
using namespace solidity::frontend;
using namespace solidity::yul;
using namespace solidity::langutil;
using namespace solidity::util;

namespace
{
Dialect const& languageToDialect(YulStack::Language _language, EVMVersion _version)
{
	switch (_language)
	{
	case YulStack::Language::Assembly:
	case YulStack::Language::StrictAssembly:
		return EVMDialect::strictAssemblyForEVMObjects(_version);
	case YulStack::Language::Yul:
		return EVMDialectTyped::instance(_version);
	}
	yulAssert(false);
	util::unreachable();
}

}

CharStream const& YulStack::charStream(std::string const& _sourceName) const
{
	yulAssert(m_charStream, "");
	yulAssert(m_charStream->name() == _sourceName, "");
	return *m_charStream;
}

bool YulStack::parse(std::string const& _sourceName, std::string const& _source)
{
	yulAssert(m_stackState == Empty);
	try
	{
		m_charStream = std::make_unique<CharStream>(_source, _sourceName);
		std::shared_ptr<Scanner> scanner = std::make_shared<Scanner>(*m_charStream);
		m_parserResult = ObjectParser(m_errorReporter, languageToDialect(m_language, m_evmVersion)).parse(scanner, false);
	}
	catch (UnimplementedFeatureError const& _error)
	{
		reportUnimplementedFeatureError(_error);
	}

	if (m_errorReporter.errors().empty())
		m_stackState = Parsed;

	return m_stackState == Parsed;
}

bool YulStack::parseAndAnalyze(std::string const& _sourceName, std::string const& _source)
{
	m_errors.clear();
	yulAssert(m_stackState == Empty);

	if (!parse(_sourceName, _source))
		return false;

	yulAssert(m_stackState == Parsed);
	yulAssert(m_parserResult, "");
	yulAssert(m_parserResult->hasCode());

	return analyzeParsed();
}

void YulStack::optimize()
{
	yulAssert(m_stackState >= AnalysisSuccessful, "Analysis was not successful.");
	yulAssert(m_parserResult);

	try
	{
		if (
			!m_optimiserSettings.runYulOptimiser &&
			yul::MSizeFinder::containsMSize(languageToDialect(m_language, m_evmVersion), *m_parserResult)
		)
			return;

		m_stackState = Parsed;
		optimize(*m_parserResult, true);

		// Optimizer does not maintain correct native source locations in the AST.
		// We can work around it by regenerating the AST from scratch from optimized IR.
		reparse();
	}
	catch (UnimplementedFeatureError const& _error)
	{
		reportUnimplementedFeatureError(_error);
	}
}

bool YulStack::analyzeParsed()
{
	yulAssert(m_stackState >= Parsed);
	yulAssert(m_parserResult, "");
	return analyzeParsed(*m_parserResult);
}

bool YulStack::analyzeParsed(Object& _object)
{
	yulAssert(m_stackState >= Parsed);
	yulAssert(_object.hasCode());
	_object.analysisInfo = std::make_shared<AsmAnalysisInfo>();

	AsmAnalyzer analyzer(
		*_object.analysisInfo,
		m_errorReporter,
		languageToDialect(m_language, m_evmVersion),
		{},
		_object.qualifiedDataNames()
	);

	bool success = false;
	try
	{
		success = analyzer.analyze(_object.code()->root());
		for (auto& subNode: _object.subObjects)
			if (auto subObject = dynamic_cast<Object*>(subNode.get()))
				if (!analyzeParsed(*subObject))
					success = false;
	}
	catch (UnimplementedFeatureError const& _error)
	{
		reportUnimplementedFeatureError(_error);
		success = false;
	}

	if (success)
		m_stackState = AnalysisSuccessful;

	return success;
}

void YulStack::compileEVM(AbstractAssembly& _assembly, bool _optimize) const
{
	EVMDialect const* dialect = nullptr;
	switch (m_language)
	{
		case Language::Assembly:
		case Language::StrictAssembly:
			dialect = &EVMDialect::strictAssemblyForEVMObjects(m_evmVersion);
			break;
		case Language::Yul:
			dialect = &EVMDialectTyped::instance(m_evmVersion);
			break;
		default:
			yulAssert(false, "Invalid language.");
			break;
	}

	EVMObjectCompiler::compile(*m_parserResult, _assembly, *dialect, _optimize, m_eofVersion);
}

void YulStack::optimize(Object& _object, bool _isCreation)
{
	yulAssert(_object.hasCode(), "");
	yulAssert(_object.analysisInfo, "");
	for (auto& subNode: _object.subObjects)
		if (auto subObject = dynamic_cast<Object*>(subNode.get()))
		{
			bool isCreation = !boost::ends_with(subObject->name, "_deployed");
			optimize(*subObject, isCreation);
		}

	Dialect const& dialect = languageToDialect(m_language, m_evmVersion);
	std::unique_ptr<GasMeter> meter;
	if (EVMDialect const* evmDialect = dynamic_cast<EVMDialect const*>(&dialect))
		meter = std::make_unique<GasMeter>(*evmDialect, _isCreation, m_optimiserSettings.expectedExecutionsPerDeployment);

	auto [optimizeStackAllocation, yulOptimiserSteps, yulOptimiserCleanupSteps] = [&]() -> std::tuple<bool, std::string, std::string>
	{
		if (!m_optimiserSettings.runYulOptimiser)
		{
			// Yul optimizer disabled, but empty sequence (:) explicitly provided
			if (OptimiserSuite::isEmptyOptimizerSequence(m_optimiserSettings.yulOptimiserSteps + ":" + m_optimiserSettings.yulOptimiserCleanupSteps))
				return std::make_tuple(true, "", "");
			// Yul optimizer disabled, and no sequence explicitly provided (assumes default sequence)
			else
			{
				yulAssert(
					m_optimiserSettings.yulOptimiserSteps == OptimiserSettings::DefaultYulOptimiserSteps &&
					m_optimiserSettings.yulOptimiserCleanupSteps == OptimiserSettings::DefaultYulOptimiserCleanupSteps
				);
				return std::make_tuple(true, "u", "");
			}

		}
		return std::make_tuple(
			m_optimiserSettings.optimizeStackAllocation,
			m_optimiserSettings.yulOptimiserSteps,
			m_optimiserSettings.yulOptimiserCleanupSteps
		);
	}();

	OptimiserSuite::run(
		dialect,
		meter.get(),
		_object,
		// Defaults are the minimum necessary to avoid running into "Stack too deep" constantly.
		optimizeStackAllocation,
		yulOptimiserSteps,
		yulOptimiserCleanupSteps,
		_isCreation ? std::nullopt : std::make_optional(m_optimiserSettings.expectedExecutionsPerDeployment),
		{}
	);
}

void YulStack::reparse()
{
	yulAssert(m_parserResult);
	yulAssert(m_charStream);

	// NOTE: Without passing in _soliditySourceProvider, printed debug info will not include code
	// snippets, but it does not matter - we'll still get the same AST after we parse it. Snippets
	// are not stored in the AST and the other info that is (location, AST ID, etc) will still be present.
	std::string source = print(nullptr /* _soliditySourceProvider */);

	YulStack cleanStack(m_evmVersion, m_eofVersion, m_language, m_optimiserSettings, m_debugInfoSelection);
	bool reanalysisSuccessful = cleanStack.parseAndAnalyze(m_charStream->name(), source);
	yulAssert(
		reanalysisSuccessful,
		source + "\n\n"
		"Invalid IR generated:\n" +
		SourceReferenceFormatter::formatErrorInformation(cleanStack.errors(), cleanStack) + "\n"
	);

	m_stackState = AnalysisSuccessful;
	m_parserResult = std::move(cleanStack.m_parserResult);

	// NOTE: We keep the char stream, and errors, even though they no longer match the object,
	// because it's the original source that matters to the user. Optimized code may have different
	// locations and fewer warnings.
}

MachineAssemblyObject YulStack::assemble(Machine _machine)
{
	yulAssert(m_stackState >= AnalysisSuccessful);
	yulAssert(m_parserResult, "");
	yulAssert(m_parserResult->hasCode(), "");
	yulAssert(m_parserResult->analysisInfo, "");

	switch (_machine)
	{
	case Machine::EVM:
		return assembleWithDeployed().first;
	}
	unreachable();
}

std::pair<MachineAssemblyObject, MachineAssemblyObject>
YulStack::assembleWithDeployed(std::optional<std::string_view> _deployName)
{
	auto [creationAssembly, deployedAssembly] = assembleEVMWithDeployed(_deployName);
	yulAssert(creationAssembly, "");
	yulAssert(m_charStream, "");

	MachineAssemblyObject creationObject;
	MachineAssemblyObject deployedObject;
	try
	{
		creationObject.bytecode = std::make_shared<evmasm::LinkerObject>(creationAssembly->assemble());
		yulAssert(creationObject.bytecode->immutableReferences.empty(), "Leftover immutables.");
		creationObject.assembly = creationAssembly;
		creationObject.sourceMappings = std::make_unique<std::string>(
			evmasm::AssemblyItem::computeSourceMapping(
				creationAssembly->items(),
				{{m_charStream->name(), 0}}
			)
		);

		if (deployedAssembly)
		{
			deployedObject.bytecode = std::make_shared<evmasm::LinkerObject>(deployedAssembly->assemble());
			deployedObject.assembly = deployedAssembly;
			deployedObject.sourceMappings = std::make_unique<std::string>(
				evmasm::AssemblyItem::computeSourceMapping(
					deployedAssembly->items(),
					{{m_charStream->name(), 0}}
				)
			);
		}
	}
	catch (UnimplementedFeatureError const& _error)
	{
		reportUnimplementedFeatureError(_error);
	}

	return {std::move(creationObject), std::move(deployedObject)};
}

std::pair<std::shared_ptr<evmasm::Assembly>, std::shared_ptr<evmasm::Assembly>>
YulStack::assembleEVMWithDeployed(std::optional<std::string_view> _deployName)
{
	yulAssert(m_stackState >= AnalysisSuccessful);
	yulAssert(m_parserResult, "");
	yulAssert(m_parserResult->hasCode(), "");
	yulAssert(m_parserResult->analysisInfo, "");

	evmasm::Assembly assembly(m_evmVersion, true, {});
	EthAssemblyAdapter adapter(assembly);

	// NOTE: We always need stack optimization when Yul optimizer is disabled (unless code contains
	// msize). It being disabled just means that we don't use the full step sequence. We still run
	// it with the minimal steps required to avoid "stack too deep".
	bool optimize = m_optimiserSettings.optimizeStackAllocation || (
		!m_optimiserSettings.runYulOptimiser &&
		!yul::MSizeFinder::containsMSize(languageToDialect(m_language, m_evmVersion), *m_parserResult)
	);
	try
	{
		compileEVM(adapter, optimize);

		assembly.optimise(evmasm::Assembly::OptimiserSettings::translateSettings(m_optimiserSettings, m_evmVersion));

		std::optional<size_t> subIndex;

		// Pick matching assembly if name was given
		if (_deployName.has_value())
		{
			for (size_t i = 0; i < assembly.numSubs(); i++)
				if (assembly.sub(i).name() == _deployName)
				{
					subIndex = i;
					break;
				}

			solAssert(subIndex.has_value(), "Failed to find object to be deployed.");
		}
		// Otherwise use heuristic: If there is a single sub-assembly, this is likely the object to be deployed.
		else if (assembly.numSubs() == 1)
			subIndex = 0;

		if (subIndex.has_value())
		{
			evmasm::Assembly& runtimeAssembly = assembly.sub(*subIndex);
			return {std::make_shared<evmasm::Assembly>(assembly), std::make_shared<evmasm::Assembly>(runtimeAssembly)};
		}
	}
	catch (UnimplementedFeatureError const& _error)
	{
		reportUnimplementedFeatureError(_error);
	}

	return {std::make_shared<evmasm::Assembly>(assembly), {}};
}

std::string YulStack::print(
	CharStreamProvider const* _soliditySourceProvider
) const
{
	yulAssert(m_stackState >= Parsed);
	yulAssert(m_parserResult, "");
	yulAssert(m_parserResult->hasCode(), "");
	return m_parserResult->toString(
		languageToDialect(m_language, m_evmVersion),
		AsmPrinter::TypePrinting::OmitDefault,
		m_debugInfoSelection,
		_soliditySourceProvider
	) + "\n";
}

Json YulStack::astJson() const
{
	yulAssert(m_stackState >= Parsed);
	yulAssert(m_parserResult, "");
	yulAssert(m_parserResult->hasCode(), "");
	return  m_parserResult->toJson();
}

std::shared_ptr<Object> YulStack::parserResult() const
{
	yulAssert(m_stackState >= AnalysisSuccessful, "Analysis was not successful.");
	yulAssert(m_parserResult, "");
	yulAssert(m_parserResult->hasCode(), "");
	return m_parserResult;
}

void YulStack::reportUnimplementedFeatureError(UnimplementedFeatureError const& _error)
{
	solAssert(_error.comment(), "Unimplemented feature errors must include a message for the user");
	m_errorReporter.unimplementedFeatureError(1920_error, _error.sourceLocation(), *_error.comment());
}
