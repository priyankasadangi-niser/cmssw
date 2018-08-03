#include <unordered_set>
#include <unordered_map>
#include "L1Trigger/L1THGCal/interface/be_algorithms/HGCalClusteringImpl.h"
#include "DataFormats/Common/interface/PtrVector.h"
#include "DataFormats/Common/interface/OrphanHandle.h"

//class constructor
HGCalClusteringImpl::HGCalClusteringImpl(const edm::ParameterSet & conf):
    siliconSeedThreshold_(conf.getParameter<double>("seeding_threshold_silicon")),
    siliconTriggerCellThreshold_(conf.getParameter<double>("clustering_threshold_silicon")),
    scintillatorSeedThreshold_(conf.getParameter<double>("seeding_threshold_scintillator")),
    scintillatorTriggerCellThreshold_(conf.getParameter<double>("clustering_threshold_scintillator")),
    dr_(conf.getParameter<double>("dR_cluster")),
    clusteringAlgorithmType_(conf.getParameter<string>("clusterType")),
    calibSF_(conf.getParameter<double>("calibSF_cluster")),
    layerWeights_(conf.getParameter< std::vector<double> >("layerWeights")),
    applyLayerWeights_(conf.getParameter< bool >("applyLayerCalibration"))
{    
    edm::LogInfo("HGCalClusterParameters") << "C2d Clustering Algorithm selected : " << clusteringAlgorithmType_ ; 
    edm::LogInfo("HGCalClusterParameters") << "C2d silicon seeding Thr: " << siliconSeedThreshold_ ; 
    edm::LogInfo("HGCalClusterParameters") << "C2d silicon clustering Thr: " << siliconTriggerCellThreshold_ ; 
    edm::LogInfo("HGCalClusterParameters") << "C2d scintillator seeding Thr: " << scintillatorSeedThreshold_ ; 
    edm::LogInfo("HGCalClusterParameters") << "C2d scintillator clustering Thr: " << scintillatorTriggerCellThreshold_ ; 
    edm::LogInfo("HGCalClusterParameters") << "C2d global calibration factor: " << calibSF_;
}


/* dR-algorithms */
bool HGCalClusteringImpl::isPertinent( const l1t::HGCalTriggerCell & tc, 
                                       const l1t::HGCalCluster & clu, 
                                       double distXY ) const 
{

    HGCalDetId tcDetId( tc.detId() );
    HGCalDetId cluDetId( clu.detId() );
    if( (tcDetId.layer() != cluDetId.layer()) ||
        (tcDetId.subdetId() != cluDetId.subdetId()) ||
        (tcDetId.zside() != cluDetId.zside()) ){
        return false;
    }   
    if ( clu.distance((tc)) < distXY ){
        return true;
    }
    return false;

}


void HGCalClusteringImpl::clusterizeDR( const std::vector<edm::Ptr<l1t::HGCalTriggerCell>> & triggerCellsPtrs, 
                                        l1t::HGCalClusterBxCollection & clusters
    ){

    bool isSeed[triggerCellsPtrs.size()];
    
    /* search for cluster seeds */
    int itc(0);
    for( std::vector<edm::Ptr<l1t::HGCalTriggerCell>>::const_iterator tc = triggerCellsPtrs.begin(); tc != triggerCellsPtrs.end(); ++tc,++itc ){
        double seedThreshold = ((*tc)->subdetId()==HGCHEB ? scintillatorSeedThreshold_ : siliconSeedThreshold_);
        isSeed[itc] = ( (*tc)->mipPt() > seedThreshold) ? true : false;
    }
    
    /* clustering the TCs */
    std::vector<l1t::HGCalCluster> clustersTmp;

    itc=0;
    for( std::vector<edm::Ptr<l1t::HGCalTriggerCell>>::const_iterator tc = triggerCellsPtrs.begin(); tc != triggerCellsPtrs.end(); ++tc,++itc ){
        double threshold = ((*tc)->subdetId()==HGCHEB ? scintillatorTriggerCellThreshold_ : siliconTriggerCellThreshold_);
        if( (*tc)->mipPt() < threshold ){
            continue;
        }
        
        /* 
           searching for TC near the center of the cluster  
           ToBeFixed: if a tc is not a seed, but could be potencially be part of a cluster generated by a late seed, 
                      the tc will not be clusterized  
        */

        double minDist = dr_;
        int targetClu = -1;

        for(unsigned iclu=0; iclu<clustersTmp.size(); iclu++){

          if(!this->isPertinent(**tc, clustersTmp.at(iclu), dr_)) continue;

          double d = clustersTmp.at(iclu).distance(**tc);
          if(d<minDist){
            minDist = d;
            targetClu = int(iclu);
          }
        }

        if(targetClu<0 && isSeed[itc]) clustersTmp.emplace_back( *tc );
        else if(targetClu>=0) clustersTmp.at( targetClu ).addConstituent( *tc );

    }

    /* store clusters in the persistent collection */
    clusters.resize(0, clustersTmp.size());
    for( unsigned i(0); i<clustersTmp.size(); ++i ){
        calibratePt(clustersTmp.at(i));
        clusters.set( 0, i, clustersTmp.at(i) );
    }
    
    

}



/* NN-algorithms */

/* storing trigger cells into vector per layer and per endcap */
void HGCalClusteringImpl::triggerCellReshuffling( const std::vector<edm::Ptr<l1t::HGCalTriggerCell>> & triggerCellsPtrs, 
                                                  std::array< std::vector<std::vector<edm::Ptr<l1t::HGCalTriggerCell>>>,kNSides_> & reshuffledTriggerCells 
    ){

    for( const auto& tc : triggerCellsPtrs ){
        int endcap = tc->zside() == -1 ? 0 : 1 ;
        HGCalDetId tcDetId( tc->detId() );
        unsigned layer = triggerTools_.layerWithOffset(tc->detId());
        
        reshuffledTriggerCells[endcap][layer-1].emplace_back(tc);
        
    }

}


/* merge clusters that have common neighbors */
void HGCalClusteringImpl::mergeClusters( l1t::HGCalCluster & main_cluster, 
                                         const l1t::HGCalCluster & secondary_cluster ) const
{

    const std::unordered_map<uint32_t, edm::Ptr<l1t::HGCalTriggerCell>>& pertinentTC = secondary_cluster.constituents();
    
    for(const auto& id_tc : pertinentTC){
        main_cluster.addConstituent(id_tc.second);
    }

}


void HGCalClusteringImpl::NNKernel( const std::vector<edm::Ptr<l1t::HGCalTriggerCell>> & reshuffledTriggerCells,
                                    l1t::HGCalClusterBxCollection & clusters,
                                    const HGCalTriggerGeometryBase & triggerGeometry
    ){
   
    /* declaring the clusters vector */
    std::vector<l1t::HGCalCluster> clustersTmp;

    // map TC id -> cluster index in clustersTmp
    std::unordered_map<uint32_t, unsigned> cluNNmap;

    /* loop over the trigger-cells */
    for( const auto& tc_ptr : reshuffledTriggerCells ){
        double threshold = (tc_ptr->subdetId()==HGCHEB ? scintillatorTriggerCellThreshold_ : siliconTriggerCellThreshold_);
        if( tc_ptr->mipPt() < threshold ){
            continue;
        }
        
        // Check if the neighbors of that TC are already included in a cluster
        // If this is the case, add the TC to the first (arbitrary) neighbor cluster
        // Otherwise create a new cluster
        bool createNewC2d(true);
        const auto neighbors = triggerGeometry.getNeighborsFromTriggerCell(tc_ptr->detId());
        for( const auto neighbor : neighbors ){
            auto tc_cluster_itr = cluNNmap.find(neighbor);
            if(tc_cluster_itr!=cluNNmap.end()){ 
                createNewC2d = false;
                if( tc_cluster_itr->second < clustersTmp.size()){
                    clustersTmp.at(tc_cluster_itr->second).addConstituent(tc_ptr);
                    // map TC id to the existing cluster
                    cluNNmap.emplace(tc_ptr->detId(), tc_cluster_itr->second);
                }
                else{
                    throw cms::Exception("HGCTriggerUnexpected")
                        << "Trying to access a non-existing cluster. But it should exist...\n";
                }                
                break;
            }
        }
        if(createNewC2d){
            clustersTmp.emplace_back(tc_ptr);
            clustersTmp.back().setValid(true);
            // map TC id to the cluster index (size - 1)
            cluNNmap.emplace(tc_ptr->detId(), clustersTmp.size()-1);
        }
    }
    
    /* declaring the vector with possible clusters merged */
    // Merge neighbor clusters together
    for( auto& cluster1 : clustersTmp){
        // If the cluster has been merged into another one, skip it
        if( !cluster1.valid() ) continue;
        // Fill a set containing all TC included in the clusters
        // as well as all neighbor TC
        std::unordered_set<uint32_t> cluTcSet;
        for(const auto& tc_clu1 : cluster1.constituents()){ 
            cluTcSet.insert( tc_clu1.second->detId() );
            const auto neighbors = triggerGeometry.getNeighborsFromTriggerCell( tc_clu1.second->detId() );
            for(const auto neighbor : neighbors){
                cluTcSet.insert( neighbor );
            }
        }        
            
        for( auto& cluster2 : clustersTmp ){
            // If the cluster has been merged into another one, skip it
            if( cluster1.detId()==cluster2.detId() ) continue;
            if( !cluster2.valid() ) continue;
            // Check if the TC in clu2 are in clu1 or its neighbors
            // If yes, merge the second cluster into the first one
            for(const auto& tc_clu2 : cluster2.constituents()){ 
                if( cluTcSet.find(tc_clu2.second->detId())!=cluTcSet.end() ){
                    mergeClusters( cluster1, cluster2 );                    
                    cluTcSet.insert( tc_clu2.second->detId() );
                    const auto neighbors = triggerGeometry.getNeighborsFromTriggerCell( tc_clu2.second->detId() );
                    for(const auto neighbor : neighbors){
                        cluTcSet.insert( neighbor );
                    }                    
                    cluster2.setValid(false);
                    break;
                }
            }
        }
    }

    /* store clusters in the persistent collection */
    // only if the cluster contain a TC above the seed threshold
    for( auto& cluster : clustersTmp ){
        if( !cluster.valid() ) continue;
        bool saveInCollection(false);
        for( const auto& id_tc : cluster.constituents() ){
            /* threshold in transverse-mip */
            double seedThreshold = (id_tc.second->subdetId()==HGCHEB ? scintillatorSeedThreshold_ : siliconSeedThreshold_);
            if( id_tc.second->mipPt() > seedThreshold ){
                saveInCollection = true;
                break;
            }
        }
        if(saveInCollection){
            calibratePt(cluster);
            clusters.push_back( 0, cluster );
        }
    }
}


void HGCalClusteringImpl::clusterizeNN( const std::vector<edm::Ptr<l1t::HGCalTriggerCell>> & triggerCellsPtrs, 
                                        l1t::HGCalClusterBxCollection & clusters,
                                        const HGCalTriggerGeometryBase & triggerGeometry
    ){

    std::array< std::vector< std::vector<edm::Ptr<l1t::HGCalTriggerCell>>>,kNSides_> reshuffledTriggerCells; 
    unsigned layers = triggerTools_.layers(ForwardSubdetector::ForwardEmpty);
    for(unsigned side=0; side<kNSides_; side++)
    {
        reshuffledTriggerCells[side].resize(layers);
    }
    triggerCellReshuffling( triggerCellsPtrs, reshuffledTriggerCells );

    for(unsigned iec=0; iec<kNSides_; ++iec){
        for(unsigned il=0; il<layers; ++il){
            NNKernel( reshuffledTriggerCells[iec][il], clusters, triggerGeometry );
        }
    }

}



/*** FW-algorithms ***/
void HGCalClusteringImpl::clusterizeDRNN( const std::vector<edm::Ptr<l1t::HGCalTriggerCell>> & triggerCellsPtrs, 
                                        l1t::HGCalClusterBxCollection & clusters,
                                        const HGCalTriggerGeometryBase & triggerGeometry
    ){

    bool isSeed[triggerCellsPtrs.size()];
    std::vector<unsigned> seedPositions;
    seedPositions.reserve( triggerCellsPtrs.size() );

    /* search for cluster seeds */
    int itc(0);
    for( std::vector<edm::Ptr<l1t::HGCalTriggerCell>>::const_iterator tc = triggerCellsPtrs.begin(); tc != triggerCellsPtrs.end(); ++tc,++itc ){
        
        double seedThreshold = ((*tc)->subdetId()==HGCHEB ? scintillatorSeedThreshold_ : siliconSeedThreshold_);

        /* decide if is a seed, if yes store the position into of triggerCellsPtrs */
        isSeed[itc] = ( (*tc)->mipPt() > seedThreshold) ? true : false;
        if( isSeed[itc] ) {

            seedPositions.push_back( itc );

            /* remove tc from the seed vector if is a NN of an other seed*/
            for( auto pos : seedPositions ){
                if( ( (*tc)->position() - triggerCellsPtrs[pos]->position() ).mag()  < dr_ ){
                    if( this->areTCneighbour( (*tc)->detId(), triggerCellsPtrs[pos]->detId(), triggerGeometry ) )
                    {
                        isSeed[itc] = false;
                        seedPositions.pop_back();
                    }
                }
            } 
        }

    }
    
    /* clustering the TCs */
    std::vector<l1t::HGCalCluster> clustersTmp;

    // every seed generates a cluster
    for( auto pos : seedPositions ) {
        clustersTmp.emplace_back( triggerCellsPtrs[pos] );
    }

    /* add the tc to the clusters */
    itc=0;
    for( std::vector<edm::Ptr<l1t::HGCalTriggerCell>>::const_iterator tc = triggerCellsPtrs.begin(); tc != triggerCellsPtrs.end(); ++tc,++itc ){
      
        /* get the correct threshold for the different part of the detector */ 
        double threshold = ((*tc)->subdetId()==HGCHEB ? scintillatorTriggerCellThreshold_ : siliconTriggerCellThreshold_);
        
        /* continue if not passing the threshold */
        if( (*tc)->mipPt() < threshold ) continue;
        if( isSeed[itc] ) continue; //No sharing of seeds between clusters (TBC)
        
        /* searching for TC near the centre of the cluster  */
        std::vector<unsigned> tcPertinentClusters; 
        unsigned iclu(0);
        
        for ( const auto& clu : clustersTmp ) {
            if( this->isPertinent(**tc, clu, dr_) ){
                tcPertinentClusters.push_back( iclu );
            }
            ++iclu;
        }   

        if ( tcPertinentClusters.empty() ) {
            continue;
        }
        else if( tcPertinentClusters.size() == 1 ) {
            clustersTmp.at( tcPertinentClusters.at(0) ).addConstituent( *tc );
        }
        else {

            /* calculate the fractions */
            double totMipt = 0;
            for( auto clu : tcPertinentClusters ){
                totMipt += clustersTmp.at( clu ).seedMipPt();
            }

            for( auto clu : tcPertinentClusters ){
                double seedMipt = clustersTmp.at( clu ).seedMipPt();
                clustersTmp.at( clu ).addConstituent( *tc, true, seedMipt/totMipt );
            }
        }
    }

    /* store clusters in the persistent collection */
    clusters.resize(0, clustersTmp.size());
    for( unsigned i(0); i<clustersTmp.size(); ++i ){
        this->removeUnconnectedTCinCluster( clustersTmp.at(i), triggerGeometry );
        calibratePt( clustersTmp.at(i) );
        clusters.set( 0, i, clustersTmp.at(i) );
    }

}


bool HGCalClusteringImpl::areTCneighbour(uint32_t detIDa, uint32_t detIDb, const HGCalTriggerGeometryBase & triggerGeometry
    ){

    const auto neighbors = triggerGeometry.getNeighborsFromTriggerCell( detIDa );
    
    if( neighbors.find( detIDb ) != neighbors.end() ) return true;

    return false;
    
}


void HGCalClusteringImpl::removeUnconnectedTCinCluster( l1t::HGCalCluster & cluster, const HGCalTriggerGeometryBase & triggerGeometry ) {

    /* get the constituents and the centre of the seed tc (considered as the first of the constituents) */
    const std::unordered_map<uint32_t, edm::Ptr<l1t::HGCalTriggerCell>>& constituents = cluster.constituents(); 
    Basic3DVector<float> seedCentre( constituents.at(cluster.detId())->position() );
    
    /* distances from the seed */
    vector<pair<edm::Ptr<l1t::HGCalTriggerCell>,float>> distances;
    for( const auto & id_tc : constituents )
    {
        Basic3DVector<float> tcCentre( id_tc.second->position() );
        float distance = ( seedCentre - tcCentre ).mag();
        distances.push_back( pair<edm::Ptr<l1t::HGCalTriggerCell>,float>( id_tc.second, distance ) );
    }

    /* sorting (needed in order to be sure that we are skipping any tc) */
    /* FIXME: better sorting needed!!! */
    std::sort( distances.begin(), distances.end(), distanceSorter );


    /* checking if the tc is connected to the seed */
    bool toRemove[constituents.size()];
    toRemove[0] = false; // this is the seed
    for( unsigned itc=1; itc<distances.size(); itc++ ){
    
        /* get the tc under study */
        toRemove[itc] = true;
        const edm::Ptr<l1t::HGCalTriggerCell>& tcToStudy = distances[itc].first;
        
        /* compare with the tc in the cluster */
        for( unsigned itc_ref=0; itc_ref<itc; itc_ref++ ){
            if( !toRemove[itc_ref] ) {
                if( areTCneighbour( tcToStudy->detId(), distances.at( itc_ref ).first->detId(), triggerGeometry ) ) {
                    toRemove[itc] = false;
                    break;
                }
            }
        }
        
    }


    /* remove the unconnected TCs */
    for( unsigned i=0; i<distances.size(); i++){
        if( toRemove[i] ) cluster.removeConstituent( distances.at( i ).first );
    }
    
}



void HGCalClusteringImpl::calibratePt( l1t::HGCalCluster & cluster ){

    double calibPt=0.;

    if(applyLayerWeights_){

        unsigned layerN = triggerTools_.layerWithOffset(cluster.detId());

        if(layerWeights_.at(layerN)==0.){
            throw cms::Exception("BadConfiguration")
                <<"2D cluster energy forced to 0 by calibration coefficients.\n"
                <<"The configuration should be changed. "
                <<"Discarded layers should be defined in hgcalTriggerGeometryESProducer.TriggerGeometry.DisconnectedLayers and not with calibration coefficients = 0\n";
        }

        calibPt = layerWeights_.at(layerN) * cluster.mipPt();

    }

    else{

        calibPt = cluster.pt() * calibSF_;

    }

    math::PtEtaPhiMLorentzVector calibP4( calibPt,
                                          cluster.eta(),
                                          cluster.phi(),
                                          0. );

    cluster.setP4( calibP4 );

}
