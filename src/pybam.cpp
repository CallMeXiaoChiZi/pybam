#include <iostream>
#include <iomanip>
#include <vector>
#include <queue>

#include <boost/foreach.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_utility.hpp>
#include <boost/graph/incremental_components.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/pending/disjoint_sets.hpp>
#include <boost/graph/strong_components.hpp>

#include <boost/python/class.hpp>
#include <boost/python/module.hpp>
#include <boost/python/def.hpp>
#include <boost/python/list.hpp>
#include <boost/python/tuple.hpp>

#include "bamtools/src/api/BamReader.h"
#include "bamtools/src/utils/bamtools_pileup_engine.h"

using namespace std;
using namespace boost;
using namespace BamTools;


python::tuple CreatePileupTuple(const PileupPosition& pileupData)
{
	int ntData[5][6] = {{0}};
	int ambiguous = 0;
	
	for (vector<PileupAlignment>::const_iterator pileupIter = pileupData.PileupAlignments.begin(); pileupIter != pileupData.PileupAlignments.end(); ++pileupIter)
	{
		const PileupAlignment& pa = (*pileupIter);
		const BamAlignment& ba = pa.Alignment;
		
		if (pa.IsCurrentDeletion)
		{
			continue;
		}
		
		char base = toupper(ba.QueryBases.at(pa.PositionInAlignment));
		
		if (base == 'N')
		{
			ambiguous++;
			continue;
		}
		
		int baseIdx;
		switch (base)
		{
			case 'A': baseIdx = 0; break;
			case 'C': baseIdx = 1; break;
			case 'G': baseIdx = 2; break;
			case 'T': baseIdx = 3; break;
			default: throw runtime_error("unrecognized base " + string(1, base));
		}
		
		// count
		ntData[baseIdx][0]++;
		ntData[4][0]++;
		
		// quality
		ntData[baseIdx][1] += ba.Qualities.at(pa.PositionInAlignment);
		ntData[4][1] += ba.Qualities.at(pa.PositionInAlignment);
		
		// mapping quality
		ntData[baseIdx][2] += ba.MapQuality;
		ntData[4][2] += ba.MapQuality;
		
		// distance
		ntData[baseIdx][3] += (ba.IsReverseStrand()) ? ba.Length - pa.PositionInAlignment - 1 : pa.PositionInAlignment;
		ntData[4][3] += (ba.IsReverseStrand()) ? ba.Length - pa.PositionInAlignment - 1 : pa.PositionInAlignment;
		
		// direction
		ntData[baseIdx][3] += (ba.IsReverseStrand()) ? 1 : 0;
		ntData[4][3] += (ba.IsReverseStrand()) ? 1 : 0;
	}
	
	// Identify major base
	int majorBaseIdx = 0;
	for (int baseIdx = 0; baseIdx < 4; baseIdx++)
	{
		if (ntData[baseIdx][0] > ntData[majorBaseIdx][0])
		{
			majorBaseIdx = baseIdx;
		}
	}
	
	// Identify minor base, initialize to base that is not the major base
	int minorBaseIdx = (majorBaseIdx + 1) % 4;
	for (int baseIdx = 0; baseIdx < 4; baseIdx++)
	{
		if (ntData[baseIdx][0] > ntData[minorBaseIdx][0] && baseIdx != majorBaseIdx)
		{
			majorBaseIdx = baseIdx;
		}
	}
	
	// Calculate entropy
	double depth = (double)pileupData.PileupAlignments.size(); // NOTE: COPIED FROM JEFF, THIS MAY BE A WRONG
	double entropy = 0.0;
	for (int baseIdx = 0; baseIdx < 4; baseIdx++)
	{
		double probability = (double)ntData[baseIdx][0] / depth;
		if (probability != 0)
		{
			entropy -= (log(probability) * probability);
		}
	}
	
	// Interface is 1-based, bamtools is 0-based
	int position = pileupData.Position + 1;
	
	return python::make_tuple(pileupData.RefId, position,
							  python::make_tuple(ntData[0][0], ntData[0][1], ntData[0][2], ntData[0][3], ntData[0][4], ntData[0][5]),
							  python::make_tuple(ntData[1][0], ntData[1][1], ntData[1][2], ntData[1][3], ntData[1][4], ntData[1][5]),
							  python::make_tuple(ntData[2][0], ntData[2][1], ntData[2][2], ntData[2][3], ntData[2][4], ntData[2][5]),
							  python::make_tuple(ntData[3][0], ntData[3][1], ntData[3][2], ntData[3][3], ntData[3][4], ntData[3][5]),
							  python::make_tuple(ntData[4][0], ntData[4][1], ntData[4][2], ntData[4][3], ntData[4][4], ntData[4][5]),
							  majorBaseIdx,
							  minorBaseIdx,
							  ambiguous,
							  0,
							  entropy,
							  0);
}


struct PileupQueue : PileupVisitor
{
	PileupQueue() : StartPosition(-1) {}
	
	void Visit(const PileupPosition& pileupData)
	{
		// Dont store tuples before the start position
		if (pileupData.Position < StartPosition)
		{
			return;
		}
		
		Pileups.push(CreatePileupTuple(pileupData));
		
		// Reset start position to have no further effect
		StartPosition = -1;
	}
	
	void Clear()
	{
		std::queue<python::tuple> empty;
		swap(Pileups, empty);
	}
	
	std::queue<python::tuple> Pileups;
	int StartPosition;
};

class Pileup
{
public:
	Pileup()
	{
		m_PileupEngine.AddVisitor(&m_PileupQueue);
	}
	
	void Open(const string& bamFilename)
	{
		if (!m_BamReader.Open(bamFilename))
		{
			throw runtime_error("unable to open bam file " + bamFilename);
		}
		
		if (!m_BamReader.LocateIndex())
		{
			throw runtime_error("unable to open index for bam file " + bamFilename);
		}
		
		m_PileupEngine.Flush();
		m_PileupQueue.Clear();
		
		RefNames = python::list();
		for (RefVector::const_iterator refDataIter = m_BamReader.GetReferenceData().begin(); refDataIter != m_BamReader.GetReferenceData().end(); refDataIter++)
		{
			RefNames.append(refDataIter->RefName);
		}
	}
	
	void Jump(const string& refName, int position)
	{
		// Interface is 1-based, bamtools is 0-based
		position -= 1;
		
		int refId = m_BamReader.GetReferenceID(refName);
		
		if (refId < 0)
		{
			throw runtime_error("invalid ref name " + refName);
		}
		
		m_PileupEngine.Flush();
		m_PileupQueue.Clear();
		
		m_BamReader.Jump(refId, position);
		
		m_PileupQueue.StartPosition = position;
		
		BamAlignment al;
		while (m_BamReader.GetNextAlignment(al))
		{
			m_PileupEngine.AddAlignment(al);
			
			if (m_PileupQueue.Pileups.empty())
			{
				continue;
			}
			
			// Remove positions before our target
			while (!m_PileupQueue.Pileups.empty() && m_PileupQueue.Pileups.front()[0] == refId && m_PileupQueue.Pileups.front()[1] < position)
			{
				m_PileupQueue.Pileups.pop();
			}
			
			// Check if we have hit or passed our target
			if (!m_PileupQueue.Pileups.empty() && (m_PileupQueue.Pileups.front()[0] != refId || m_PileupQueue.Pileups.front()[1] >= position))
			{
				break;
			}
		}
	}
	
	python::object Next()
	{
		BamAlignment al;
		while (m_BamReader.GetNextAlignment(al))
		{
			m_PileupEngine.AddAlignment(al);
			
			if (!m_PileupQueue.Pileups.empty())
			{
				break;
			}
		}
		
		if (m_PileupQueue.Pileups.empty())
		{
			return python::object();
		}
		
		python::tuple tpl;
		swap(tpl, m_PileupQueue.Pileups.front());
		m_PileupQueue.Pileups.pop();
		
		return tpl;
	}
	
	python::list RefNames;
	
private:
	BamReader m_BamReader;
	PileupEngine m_PileupEngine;
	PileupQueue m_PileupQueue;
};


BOOST_PYTHON_MODULE(pybam)
{
	using namespace python;
	
	class_<Pileup>("pileup", init<>())
		.def_readonly("refnames", &Pileup::RefNames)
		.def("open", &Pileup::Open)
		.def("jump", &Pileup::Jump)
		.def("next", &Pileup::Next)
	;
}

