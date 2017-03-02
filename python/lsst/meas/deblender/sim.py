from __future__ import print_function, division
from collections import OrderedDict
import logging

import numpy as np
import scipy.spatial
import matplotlib
import matplotlib.pyplot as plt
from astropy.table import Table as ApTable

import lsst.afw.table as afwTable
import lsst.afw.math as afwMath

from . import utils as debUtils
from . import baseline

logging.basicConfig()
logger = logging.getLogger("lsst.meas.deblender")

def loadSimCatalog(filename):
    """Load a catalog of galaxies generated by galsim
    
    This can be used to ensure that the deblender is correctly deblending objects
    """
    cat = afwTable.BaseCatalog.readFits(filename)
    columns = []
    names = []
    for col in cat.getSchema().getNames():
        names.append(col)
        columns.append(cat.columns.get(col))
    catTable = ApTable(columns, names=tuple(names))
    return cat, catTable

def getNoise(calexps):
    avgNoise = []
    for calexp in calexps:
        var = calexp.getMaskedImage().getVariance()
        mask = calexp.getMaskedImage().getMask()
        stats = afwMath.makeStatistics(var, mask, afwMath.MEDIAN)
        avgNoise.append(np.sqrt(stats.getValue(afwMath.MEDIAN)))
    return avgNoise

def buildPeakTable(expDb, filters):
    parents = []
    peakIdx = []
    peaks = []
    x = []
    y = []
    blends = []
    footprints = []
    for src in expDb.mergedDet:
        sid = src.getId()
        footprint = src.getFootprint()
        if len(footprint.getPeaks())>=2:
            blended = True
        else:
            blended = False
        for pk, peak in enumerate(footprint.getPeaks()):
            parents.append(sid)
            peakIdx.append(pk)
            peaks.append(peak)
            x.append(peak.getIx())
            y.append(peak.getIy())
            blends.append(blended)
            footprints.append(footprint)
    peakTable = ApTable([parents, peakIdx, x, y, blends, peaks, footprints],
                        names=("parent", "peakIdx", "x", "y", "blended", "peak", "parent footprint"))
    # Create empty columns to hold fluxes
    for f in filters:
        peakTable["flux_"+f] = np.nan
    
    return peakTable

def matchToRef(peakTable, simTable, filters, maxSeparation=3, poolSize=-1, avgNoise=None,
               display=True, calexp=None):
    # Create arrays that scipy.spatial.cKDTree can recognize and find matches for each peak
    peakCoords = np.array(list(zip(peakTable['x'], peakTable['y'])))
    simCoords = np.array(list(zip(simTable['x'], simTable['y'])))
    kdtree = scipy.spatial.cKDTree(simCoords)
    d2, idx = kdtree.query(peakCoords, n_jobs=poolSize)
    # Only consider matches less than the maximum separation
    matched = d2<maxSeparation
    # Check to see if any peaks are matched with the same reference source
    unique, uniqueInv, uniqueCounts = np.unique(idx[matched], return_inverse=True, return_counts=True)
    # Create a table with sim information matched to each peak
    matchTable = simTable[idx]
    matchTable["matched"] = matched
    matchTable["distance"] = d2
    matchTable[~matched] = [None]*len(matchTable.colnames)
    matchTable["duplicate"] = False
    matchTable["duplicate"][matched] = uniqueCounts[uniqueInv]>1

    # Display information about undetected sources
    sidx = set(idx)
    srange = set(range(len(simTable)))
    unmatched = list(srange-sidx)
    logger.info("Sources not detected: {0}".format(len(unmatched)))

    if display:
        x = range(len(filters))
    
        for src in simTable[unmatched]:
            flux = np.array([src["flux_{0}".format(f)] for f in filters])
            plt.plot(x, flux, '.-', c="#4c72b0")
        plt.plot(x, flux, '.-', c="#4c72b0", label="Not Detected")
        if avgNoise is not None:
            plt.plot(x, avgNoise, '.-', c="#c44e52", label="Background")
        plt.legend(loc='center left', bbox_to_anchor=(1, .5),
                   fancybox=True, shadow=True)
        plt.xticks([-.25]+x+[x[-1]+.25], [""]+[f for f in filters]+[""])
        plt.xlabel("Filter")
        plt.ylabel("Total Flux")
        plt.show()

    if calexp is not None:
        
        unmatched = peakTable[~matchTable["matched"]]
        unmatchedParents = np.unique(unmatched["parent"])
        
        for pid in unmatchedParents:
            footprint = peakTable[peakTable["parent"]==pid][0]["parent footprint"]
            bbox = footprint.getBBox()
            img = debUtils.extractImage(calexp.getMaskedImage(), bbox)
            vmin, vmax = debUtils.zscale(img)
            plt.imshow(img, vmin=vmin, vmax=10*vmax)
            xmin = bbox.getMinX()
            ymin = bbox.getMinY()
            xmax = xmin+bbox.getWidth()
            ymax = ymin+bbox.getHeight()

            peakCuts = ((peakTable["x"]>xmin) &
                       (peakTable["x"]<xmax) &
                       (peakTable["y"]>ymin) &
                       (peakTable["y"]<ymax))
            goodX = peakTable["x"][peakCuts & matchTable["matched"]]
            goodY = peakTable["y"][peakCuts & matchTable["matched"]]
            badX = peakTable["x"][peakCuts & ~matchTable["matched"]]
            badY = peakTable["y"][peakCuts & ~matchTable["matched"]]
            plt.plot(goodX-xmin, goodY-ymin, 'gx', mew=2)

            simCuts = ((simTable['x']>=xmin) &
                       (simTable['x']<=xmax) &
                       (simTable['y']>=ymin) &
                       (simTable['y']<=ymax))
            simx = simTable['x'][simCuts]-xmin
            simy = simTable['y'][simCuts]-ymin
            plt.plot(simx, simy, 'o', ms=20, mec='c', mfc='none')

            plt.plot(badX-xmin, badY-ymin, 'rx', mew=2)
            plt.xlim([0, bbox.getWidth()])
            plt.ylim([0, bbox.getHeight()])
            plt.show()
    return matchTable, idx

def deblendSimExposuresOld(filters, expDb, peakTable=None):
    plugins = baseline.DEFAULT_PLUGINS
    maskedImages = [calexp.getMaskedImage() for calexp in expDb.calexps]
    psfs = [calexp.getPsf() for calexp in expDb.calexps]
    fwhm = [psf.computeShape().getDeterminantRadius() * 2.35 for psf in psfs]
    blends = expDb.mergedTable["peaks"]>1
    deblenderResults = OrderedDict()
    parents = OrderedDict()
    for n, blend in enumerate(expDb.mergedDet[blends]):
        parents[blend.getId()] = blend
        logger.debug("Deblending blend {0}".format(n))
        footprint = blend.getFootprint()
        footprints = [footprint]*len(expDb.calexps)
        deblenderResult = baseline.newDeblend(plugins, footprints, maskedImages, psfs, fwhm, filters=filters)
        deblenderResults[blend.getId()] = deblenderResult
    
        if peakTable is not None:
            for p,peak in enumerate(deblenderResult.peaks):
                cuts = (peakTable["parent"]==blend.getId()) & (peakTable["peakIdx"]==p)
                for f in filters:
                    fluxPortion = peak.deblendedPeaks[f].fluxPortion.getImage().getArray()
                    peakTable["flux_"+f][cuts] = np.sum(fluxPortion)

    return deblenderResults

def displayImage(n, ratio, fidx, expDb):
    src = expDb.mergedDet[n]
    if expDb.mergedTable["peaks"][n]>=2:
        print("Multiple Peaks in image")
    mask = debUtils.getFootprintArray(src)[1].mask
    img = debUtils.extractImage(expDb.calexps[fidx].getMaskedImage(), src.getFootprint().getBBox())
    img = np.ma.array(img, mask=mask)
    plt.imshow(img)
    plt.title("Flux Difference: {0}%".format(ratio))
    plt.show()

def calculateNmfFlux(expDb, peakTable):
    for pk, peak in enumerate(peakTable):
        if peak["parent"] in expDb.deblendedParents:
            deblendedParent = expDb.deblendedParents[peak["parent"]]
            for fidx, f in enumerate(expDb.filters):
                template = deblendedParent.getTemplate(fidx, peak["peakIdx"])
                peakTable["flux_"+f][pk] = np.sum(template)

def calculateIsolatedFlux(filters, expDb, peakTable, simTable, fluxThresh=100):
    for n, peak in enumerate(peakTable):
        if peak["blended"] or ~simTable[n]["matched"]:
            continue
        footprint = peak["parent footprint"]
        mask = debUtils.getFootprintArray(footprint)[1].mask
        
        for fidx, f in enumerate(filters):
            img = debUtils.extractImage(expDb.calexps[fidx].getMaskedImage(), footprint.getBBox())
            img = np.ma.array(img, mask=mask)
            flux = np.ma.sum(img)
            peakTable["flux_"+f][n] = flux
            
            simFlux = simTable["flux_{0}".format(f)][n]
            # Display any sources with very large flux differences
            # (this should be none, since these are )
            if (flux/simFlux<.6 or simFlux/flux<.5) and simFlux>fluxThresh:
                logger.info("n: {0}, Filter: {1}, simFlux: {2}, flux: {3}".format(n, f, simFlux, flux))
                displayImage(n, int(flux/simFlux*100), fidx, expDb)

def calculateSedsFromFlux(tbl, filters):
    fluxCols = ["flux_"+f for f in filters]
    shape = (len(tbl), len(fluxCols))
    seds = tbl[fluxCols].as_array().view(np.float64).reshape(shape)
    normalization = np.sum(seds, axis=1)
    seds = seds/normalization[:,None]
    tbl["sed"] = seds
    return seds, normalization

def plotSedComparison(simTable, peakTable, nmfPeakTable, minFlux):
    matched = simTable["matched"]
    goodFlux = simTable["flux_i"][matched]>minFlux
    flux = simTable["flux_i"][matched]
    sed = simTable["sed"][matched]
    diffOld = peakTable["sed"][matched]-sed
    diffNmf = nmfPeakTable["sed"][matched]-sed
    errOld = np.sqrt(np.sum(((diffOld/sed)**2), axis=1)/len(sed[0]))
    errNmf = np.sqrt(np.sum(((diffNmf/sed)**2), axis=1)/len(sed[0]))
    
    # Plot the histogram
    plt.figure(figsize=(8,4))
    bins = np.arange(0,23,2)
    bins = [0,5,10,15,20,25]
    weight = np.ones_like(errOld[goodFlux])/len(errOld[goodFlux])
    clippedErrors = [np.clip(err*100, bins[0], bins[-1]) for err in [errOld[goodFlux], errNmf[goodFlux]]]
    plt.hist(clippedErrors, bins=bins, weights=[weight]*2, label=["LSST","NMF"])
    xlabels = [str(b) for b in bins[:-1]]
    xlabels[-1] += "+"
    plt.xticks(bins, xlabels)
    plt.title("SED")
    plt.xlabel("Error (%)")
    plt.ylabel("Fraction of Sources")
    plt.grid()
    plt.legend(fancybox=True, shadow=True, ncol=2)
    plt.show()

    # Setup the combined SED scatter plot with all sources included
    fig = plt.figure(figsize=(8,3))
    ax = fig.add_subplot(1,1,1)
    ax.set_frame_on(False)
    ax.set_xticks([])
    ax.get_yaxis().set_visible(False)
    ax.set_xlabel("Simulated Flux", labelpad=30)

    # Plot the SED scatter plots for LSST and NMF deblending
    ax = fig.add_subplot(1,2,1)
    ax.plot(flux[goodFlux], errOld[goodFlux], '.', label="LSST")
    ax.plot(flux[~goodFlux], errOld[~goodFlux], '.', label="Bad LSST")
    ax.set_ylabel("Fractional Error")
    ax.set_title("LSST", y=.85)
    ax = fig.add_subplot(1,2,2)
    ax.plot(flux[goodFlux], errNmf[goodFlux], '.', label="NMF")
    ax.plot(flux[~goodFlux], errNmf[~goodFlux], '.', label="Bad NMF")
    ax.set_title("NMF", y=.85)
    plt.show()

    # Plot the clipped SED scatter plots
    plt.figure(figsize=(8,5))
    plt.plot(flux[goodFlux], errOld[goodFlux], '+', label="LSST", alpha=.5, mew=2)
    plt.plot(flux[goodFlux], errNmf[goodFlux], '.', label="NMF", alpha=.5, color="#c44e52")
    plt.xlabel("Simulated Flux")
    plt.ylabel("Fractional Error")
    plt.legend(fancybox=True, shadow=True, ncol=2)
    plt.gca().yaxis.grid(True)
    plt.show()

def compareMeasToSim(peakTables, simTables, nmfPeakTables, filters, minFlux=50):
    from astropy.table import vstack
    for n in range(len(peakTables)):
        peakTables[n]["image"] = n+1
        simTables[n]["image"] = n+1
    peakTable = vstack(peakTables)
    simTable = vstack(simTables)
    del simTable["sed"]
    del simTable["wave"]
    nmfPeakTable = vstack(nmfPeakTables)

    # Display statistics
    #logger.info("Total Simulated Sources: {0}".format(len(simTable)))
    logger.info("Total Detected Sources: {0}".format(len(peakTable)))
    logger.info("Total Matches: {0}".format(np.sum(simTable["matched"])))
    logger.info("Matched Isolated sources: {0}".format(np.sum(simTable["matched"]&~peakTable["blended"])))
    logger.info("Matched Blended sources: {0}".format(np.sum(simTable["matched"]&peakTable["blended"])))
    logger.info("Total Duplicates: {0}".format(np.sum(simTable["duplicate"])))

    # Calculate and compare SEDs
    calculateSedsFromFlux(peakTable, filters)
    calculateSedsFromFlux(nmfPeakTable, filters)
    calculateSedsFromFlux(simTable, filters)
    plotSedComparison(simTable, peakTable, nmfPeakTable, minFlux)

    for f in filters:
        flux = "flux_"+f
        diff = (peakTable[flux]-simTable[flux])/simTable[flux]
        diffNmf = (nmfPeakTable[flux]-simTable[flux])/simTable[flux]
        matched = simTable["matched"]
        blended = peakTable["blended"]
        lowflux = peakTable[flux]<minFlux
        plt.figure(figsize=(8,4))
        plt.plot(simTable[flux][matched & blended & ~lowflux], diff[matched & blended & ~lowflux], '.', label="LSST")
        plt.plot(simTable[flux][matched & blended & ~lowflux], diffNmf[matched & blended & ~lowflux], '.', label="NMF")
        plt.plot(simTable[flux][matched & ~blended & ~lowflux], diff[matched & ~blended & ~lowflux], '.', label="Isolated")
        plt.title("Filter {0}".format(f), y=.9)
        plt.xlabel("Simulated Flux (counts)")
        plt.ylabel("Fractional Error")
        plt.legend(loc="upper center", fancybox=True, shadow=True, bbox_to_anchor=(.5, 1.2), ncol=3)
        plt.show()
        
        plt.figure(figsize=(8,4))
        bins = np.arange(0,23,2)
        bins = [0,5,10,15,20,25]
        datasets = [np.abs(diff[matched&blended&~lowflux]),
                    np.abs(diffNmf[matched&blended&~lowflux]),
                    np.abs(diff[matched&~blended&~lowflux])]
        weights = [np.ones_like(data)/len(data) for data in datasets]
        clippedErrors = [np.clip(data*100, bins[0], bins[-1]) for data in datasets]
        plt.hist(clippedErrors, bins=bins, weights=weights, label=["LSST","NMF","Isolated"])
        xlabels = [str(b) for b in bins[:-1]]
        xlabels[-1] += "+"
        plt.xticks(bins, xlabels)
        plt.title("Filter {0} Flux".format(f), y=.9)
        plt.xlabel("Error (%)")
        plt.ylabel("Fraction of Sources")
        plt.gca().yaxis.grid(True)
        plt.legend(loc="upper center", fancybox=True, shadow=True, ncol=3, bbox_to_anchor=(.5, 1.2))
        plt.show()
        
        logger.info("Isolated Mean: {0}".format(np.mean(np.abs(diff[matched&~blended & ~lowflux]))))
        logger.info("Isolated RMS: {0}".format(np.sqrt(np.mean(diff[matched&~blended & ~lowflux])**2+
                                                       np.std(diff[matched&~blended & ~lowflux])**2)))
        logger.info("Blended Mean: {0}".format(np.mean(np.abs(diff[matched&blended & ~lowflux]))))
        logger.info("Blended RMS: {0}".format(np.sqrt(np.mean(diff[matched&blended & ~lowflux])**2+
                                                      np.std(diff[matched&blended & ~lowflux])**2)))
    return peakTable, simTable, nmfPeakTable