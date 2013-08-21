//# Filter.cc: DPPP step class to add station to a superstation
//# Copyright (C) 2012
//# ASTRON (Netherlands Institute for Radio Astronomy)
//# P.O.Box 2, 7990 AA Dwingeloo, The Netherlands
//#
//# This file is part of the LOFAR software suite.
//# The LOFAR software suite is free software: you can redistribute it and/or
//# modify it under the terms of the GNU General Public License as published
//# by the Free Software Foundation, either version 3 of the License, or
//# (at your option) any later version.
//#
//# The LOFAR software suite is distributed in the hope that it will be useful,
//# but WITHOUT ANY WARRANTY; without even the implied warranty of
//# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//# GNU General Public License for more details.
//#
//# You should have received a copy of the GNU General Public License along
//# with the LOFAR software suite. If not, see <http://www.gnu.org/licenses/>.
//#
//# $Id$
//#
//# @author Ger van Diepen

#include <lofar_config.h>
#include <DPPP/Filter.h>
#include <DPPP/DPBuffer.h>
#include <DPPP/DPInfo.h>
#include <DPPP/DPLogger.h>
#include <Common/ParameterSet.h>

#include <tables/Tables/ScalarColumn.h>
#include <tables/Tables/TableRecord.h>
#include <tables/Tables/ExprNode.h>
#include <tables/Tables/RecordGram.h>
#include <casa/Containers/Record.h>

using namespace casa;

namespace LOFAR {
  namespace DPPP {

    Filter::Filter (DPInput* input,
                    const ParameterSet& parset,
                    const string& prefix)
      : itsInput        (input),
        itsName         (prefix),
        itsStartChanStr (parset.getString(prefix+"startchan", "0")),
        itsNrChanStr    (parset.getString(prefix+"nchan", "0")),
        itsRemoveAnt    (parset.getBool  (prefix+"remove", false)),
        itsBaselines    (parset, prefix),
        itsDoSelect     (false)
    {}

    Filter::Filter (DPInput* input, const BaselineSelection& baselines)
      : itsInput        (input),
        itsStartChanStr ("0"),
        itsNrChanStr    ("0"),
        itsRemoveAnt    (false),
        itsBaselines    (baselines),
        itsDoSelect     (false)
    {}

    Filter::~Filter()
    {}

    void Filter::updateInfo (const DPInfo& infoIn)
    {
      info() = infoIn;
      info().setNeedVisData();
      info().setNeedWrite();
      // Parse the chan expressions.
      // Nr of channels can be used as 'nchan' in the expressions.
      Record rec;
      rec.define ("nchan", infoIn.nchan());
      TableExprNode node1 (RecordGram::parse(rec, itsStartChanStr));
      TableExprNode node2 (RecordGram::parse(rec, itsNrChanStr));
      // nchan=0 means until the last channel.
      double result;
      node1.get (rec, result);
      itsStartChan = uint(result+0.001);
      node2.get (rec, result);
      uint nrChan = uint(result+0.0001);
      uint nAllChan = getInfo().nchan();
      ASSERTSTR (itsStartChan < nAllChan,
                 "startchan " << itsStartChan
                 << " exceeds nr of available channels (" << nAllChan << ')');
      uint maxNrChan = nAllChan - itsStartChan;
      if (nrChan == 0) {
        nrChan = maxNrChan;
      } else {
        nrChan = std::min (nrChan, maxNrChan);
      }
      itsDoSelect = itsStartChan>0 || nrChan<maxNrChan;
      // Handle possible baseline selection.
      if (itsBaselines.hasSelection()) {
        Matrix<bool> selbl(itsBaselines.apply (infoIn));
        const Vector<Int>& ant1 = getInfo().getAnt1();
        const Vector<Int>& ant2 = getInfo().getAnt2();
        itsSelBL.reserve (ant1.size());
        for (uint i=0; i<ant1.size(); ++i) {
          if (selbl(ant1[i], ant2[i])) {
            itsSelBL.push_back (i);
          }
        }
        if (itsSelBL.size() < ant1.size()) {
          itsDoSelect = true;
        }
      }
      if (itsDoSelect || itsRemoveAnt) {
        // Update the DPInfo object.
        info().update (itsStartChan, nrChan, itsSelBL, itsRemoveAnt);
        if (itsDoSelect) {
          // Shape the arrays in the buffer.
          IPosition shape (3, infoIn.ncorr(), nrChan, getInfo().nbaselines());
          itsBuf.getData().resize (shape);
          itsBuf.getFlags().resize (shape);
          itsBuf.getWeights().resize (shape);
        }
      }
    }

    void Filter::show (std::ostream& os) const
    {
      os << "Filter " << itsName << std::endl;
      os << "  startchan:      " << itsStartChan << "  (" << itsStartChanStr
         << ')' << std::endl;
      os << "  nchan:          " << getInfo().nchan() << "  (" << itsNrChanStr
         << ')' << std::endl;
      itsBaselines.show (os);
      os << "  remove:         " << itsRemoveAnt << std::endl;
    }

    void Filter::showTimings (std::ostream& os, double duration) const
    {
      os << "  ";
      FlagCounter::showPerc1 (os, itsTimer.getElapsed(), duration);
      os << " Filter " << itsName << endl;
    }

    bool Filter::process (const DPBuffer& buf)
    {
      itsTimer.start();
      if (!itsDoSelect) {
        itsBuf = buf;      // uses reference semantics
        itsTimer.stop();
        getNextStep()->process (itsBuf);
        return true;
      }
      // Make sure no other object references the DATA and UVW arrays.
      itsBuf.getData().unique();
      itsBuf.getFlags().unique();
      itsBuf.getWeights().unique();
      itsBuf.getFullResFlags().unique();
      // Get the various data arrays.
      RefRows rowNrs(buf.getRowNrs());
      const Array<Complex>& data = buf.getData();
      const Array<Bool>& flags = buf.getFlags();
      Array<Float> weights(itsInput->fetchWeights (buf, rowNrs, itsTimer));
      Array<Double> uvws(itsInput->fetchUVW (buf, rowNrs, itsTimer));
      Array<Bool> frFlags(itsInput->fetchFullResFlags(buf, rowNrs, itsTimer));
      int frfAvg = frFlags.shape()[0] / data.shape()[1];
      // Size fullResFlags if not done yet.
      if (itsBuf.getFullResFlags().empty()) {
        IPosition frfShp = frFlags.shape();
        frfShp[0] = getInfo().nchan() * frfAvg;
        frfShp[2] = getInfo().nbaselines();
        itsBuf.getFullResFlags().resize (frfShp);
      }
      // Form the blc and trc for the channel selection.
      IPosition first(3, 0);
      IPosition last (data.shape() - 1);
      first[1] = itsStartChan;
      last[1]  = itsStartChan + getInfo().nchan() - 1;
      IPosition frfFirst(3,0);
      IPosition frfLast (frFlags.shape() - 1);
      frfFirst[0] = first[1] * frfAvg;
      frfLast[0]  = (last[1] + 1) * frfAvg - 1;
      // Copy the data into the output buffer.
      if (itsSelBL.empty()) {
        // No baseline selection; copy all data for given channels to
        // make them contiguous.
        // UVW can be referenced, because not dependent on channel.
        itsBuf.getData() = data(first, last);
        itsBuf.getFlags() = flags(first, last);
        itsBuf.getWeights() = weights(first, last);
        itsBuf.getFullResFlags() = frFlags(frfFirst, frfLast);
        itsBuf.setUVW (buf.getUVW());
      } else {
        // Copy the data of the selected baselines and channels.
        itsBuf.getUVW().resize (IPosition(2, 3, getInfo().nbaselines()));
        itsBuf.getUVW().unique();
        Complex* toData   = itsBuf.getData().data();
        Bool*    toFlag   = itsBuf.getFlags().data();
        Float*   toWeight = itsBuf.getWeights().data();
        Double*  toUVW    = itsBuf.getUVW().data();
        Bool*    toFrf    = itsBuf.getFullResFlags().data();
        uint off = data.shape()[0] * first[1];    // offset of first channel
        const Complex* frData   = data.data()    + off;
        const Bool*    frFlag   = flags.data()   + off;
        const Float*   frWeight = weights.data() + off;
        const Double*  frUVW    = uvws.data();
        int ndfr = data.shape()[0] * data.shape()[1];
        int ndto = itsBuf.getData().shape()[0] * itsBuf.getData().shape()[1];
        int nffr = frFlags.shape()[0];
        int nfto = itsBuf.getFullResFlags().shape()[0];
        for (uint i=0; i<itsSelBL.size(); ++i) {
          objcopy (toData  , frData   + itsSelBL[i]*ndfr, ndto);
          toData += ndto;
          objcopy (toFlag  , frFlag   + itsSelBL[i]*ndfr, ndto);
          toFlag += ndto;
          objcopy (toWeight, frWeight + itsSelBL[i]*ndfr, ndto);
          toWeight += ndto;
          objcopy (toUVW   , frUVW    + itsSelBL[i]*3   , 3);
          toUVW += 3;
          // Copy FullResFlags for all times.
          const Bool* frFrf = (frFlags.data() + frfFirst[0] +
                               itsSelBL[i]*nffr * frFlags.shape()[1]);
          for (int j=0; j<=frfLast[1]; ++j) {
            objcopy (toFrf, frFrf, nfto);
            toFrf += nfto;
            frFrf += nffr;
          }
        }
      }
      itsBuf.setTime     (buf.getTime());
      itsBuf.setExposure (buf.getExposure());
      itsTimer.stop();
      getNextStep()->process (itsBuf);
      return true;
    }

    void Filter::finish()
    {
      // Let the next steps finish.
      getNextStep()->finish();
    }

    void Filter::addToMS (const string& msName)
    {
      if (! itsRemoveAnt) {
        return;
      }
      // See if and which stations have been removed.
      Table antTab (msName + "/ANTENNA", Table::Update);
      Table selTab = antTab(! antTab.col("NAME").in (info().antennaNames()));
      if (selTab.nrow() == 0) {
        return;
      }
      // Remove these rows from the ANTENNA table.
      // Note that stations of baselines that have been filtered out before,
      // will also be removed.
      Vector<uInt> removedAnt = selTab.rowNumbers();
      Vector<Int> antMap = createIdMap (antTab.nrow(), removedAnt);
      antTab.removeRow (removedAnt);
      // Remove and renumber the stations in other subtables.
      Table ms(msName);
      uInt nr;
      renumberSubTable (ms, "FEED", "ANTENNA_ID", removedAnt, antMap, nr);
      renumberSubTable (ms, "POINTING", "ANTENNA_ID", removedAnt, antMap, nr);
      renumberSubTable (ms, "SYSCAL", "ANTENNA_ID", removedAnt, antMap, nr);
      renumberSubTable (ms, "QUALITY_BASELINE_STATISTIC", "ANTENNA1",
                        removedAnt, antMap, nr);
      renumberSubTable (ms, "QUALITY_BASELINE_STATISTIC", "ANTENNA2",
                        removedAnt, antMap, nr);
      // Finally remove and renumber in the beam tables.
      uInt nrAntFldId;
      Vector<uInt> remAntFldId = renumberSubTable (ms, "LOFAR_ANTENNA_FIELD",
                                                   "ANTENNA_ID",
                                                   removedAnt, antMap,
                                                   nrAntFldId);
      if (! remAntFldId.empty()) {
        Vector<Int> antFldIdMap = createIdMap (nrAntFldId, remAntFldId);
        renumberSubTable (ms, "LOFAR_ELEMENT_FAILURE", "ANTENNA_FIELD_ID",
                          remAntFldId, antFldIdMap, nr);
      }
    }

    Vector<Int> Filter::createIdMap (uInt nrId,
                                     const Vector<uInt>& removedIds) const
    {
      // Create the mapping from old to new id.
      Vector<Int> idMap (nrId);
      indgen (idMap);   // fill with 0,1,2,...
      int nrrem = 0;
      for (uInt i=0; i<removedIds.size(); ++i) {
        idMap[removedIds[i]] = -1;
        nrrem++;
        if (i < removedIds.size() - 1) {
          for (uInt j=removedIds[i]+1; j<removedIds[i+1]; ++j) {
            idMap[j] -= nrrem;
          }
        }
      }
      for (uInt j=removedIds[removedIds.size()-1]+1; j<idMap.size(); ++j) {
        idMap[j] -= nrrem;
      }
      return idMap;
    }

    Vector<uInt> Filter::renumberSubTable (const Table& ms,
                                           const String& name,
                                           const String& colName,
                                           const Vector<uInt>& removedAnt,
                                           const Vector<Int>& antMap,
                                           uInt& nrId) const
    {
      // Exit if no such subtable.
      if (! ms.keywordSet().isDefined(name)) {
        return Vector<uInt>();
      }
      // Remove the rows of the removed stations.
      Table subTab (ms.tableName() + '/' + name, Table::Update);
      nrId = subTab.nrow();
      Table selTab = subTab(subTab.col(colName).in (removedAnt));
      subTab.removeRow (selTab.rowNumbers());
      // Renumber the rest.
      ScalarColumn<Int> antCol(subTab, colName);
      Vector<Int> antIds = antCol.getColumn();
      for (uint i=0; i<antIds.size(); ++i) {
        Int newId = antMap[antIds[i]];
        ASSERT (newId >= 0);
        antIds[i] = newId;
      }
      antCol.putColumn (antIds);
      return selTab.rowNumbers();
    }

  } //# end namespace
}
