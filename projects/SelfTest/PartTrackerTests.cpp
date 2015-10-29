/*
 *  Created by Phil on 1/10/2015.
 *  Copyright 2015 Two Blue Cubes Ltd
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#include "internal/catch_suppress_warnings.h"
#include "internal/catch_compiler_capabilities.h"
#include "internal/catch_ptr.hpp"

#ifdef __clang__
//#   pragma clang diagnostic ignored "-Wpadded"
//#   pragma clang diagnostic ignored "-Wc++98-compat"
#endif

#include <cassert>
#include <memory>
#include <vector>

namespace Catch
{
    struct IPartTracker : SharedImpl<> {
        virtual ~IPartTracker() {}
        
        // static queries
        virtual std::string name() const = 0;
        
        // dynamic queries
        virtual bool hasStarted() const = 0; // true even if ended
        virtual bool hasEnded() const = 0;
        virtual bool isSuccessfullyCompleted() const = 0;
        virtual bool isOpen() const = 0;
        
        virtual IPartTracker& parent() = 0;
        
        // actions
        virtual void close() = 0;
        virtual void fail() = 0;
        virtual void markAsNeedingAnotherRun() =0;

        virtual void addChild( Ptr<IPartTracker> const& child ) = 0;
        virtual IPartTracker* findChild( std::string const& name ) = 0;
        virtual void openChild() = 0;
    };
    

    class TrackerContext {
        
        enum RunState {
            NotStarted,
            Executing,
            CompletedCycle
        };
        
        Ptr<IPartTracker> m_rootPart;
        IPartTracker* m_currentPart;
        RunState m_runState;
        
    public:
        
        static TrackerContext& instance() {
            static TrackerContext s_instance;
            return s_instance;
        }

        TrackerContext()
        :   m_currentPart( CATCH_NULL ),
            m_runState( NotStarted )
        {}


        IPartTracker& startRun();
        
        void endRun() {
            m_rootPart.reset();
            m_currentPart = CATCH_NULL;
            m_runState = NotStarted;
        }

        void startCycle() {
            m_currentPart = m_rootPart.get();
            m_runState = Executing;
        }
        void completeCycle() {
            m_runState = CompletedCycle;
        }
        
        bool completedCycle() const {
            return m_runState == CompletedCycle;
        }
        
        IPartTracker& currentPart() {
            return *m_currentPart;
        }
        void setCurrentPart( IPartTracker* part ) {
            m_currentPart = part;
        }
        
        IPartTracker* findPart( std::string const& name ) {
            return m_currentPart->findChild( name );
        }
        
    };
    
    class PartTrackerBase : public IPartTracker {
    protected:
        enum RunState {
            NotStarted,
            Executing,
            ExecutingChildren,
            NeedsAnotherRun,
            CompletedSuccessfully,
            Failed
        };
        class TrackerHasName {
            std::string m_name;
        public:
            TrackerHasName( std::string const& name ) : m_name( name ) {}
            bool operator ()( Ptr<IPartTracker> const& tracker ) {
                return tracker->name() == m_name;
            }
        };
        typedef std::vector<Ptr<IPartTracker> > Children;
        std::string m_name;
        TrackerContext& m_ctx;
        IPartTracker* m_parent;
        Children m_children;
        RunState m_runState;
    public:
        PartTrackerBase( std::string const& name, TrackerContext& ctx, IPartTracker* parent )
        :   m_name( name ),
            m_ctx( ctx ),
            m_parent( parent ),
            m_runState( NotStarted ) {}
        
        virtual std::string name() const CATCH_OVERRIDE {
            return m_name;
        }
        
        virtual bool hasEnded() const CATCH_OVERRIDE {
            return m_runState == CompletedSuccessfully || m_runState == Failed;
        }
        virtual bool isSuccessfullyCompleted() const CATCH_OVERRIDE {
            return m_runState == CompletedSuccessfully;
        }
        virtual bool hasStarted() const CATCH_OVERRIDE {
            return m_runState != NotStarted;
        }
        virtual bool isOpen() const CATCH_OVERRIDE {
            return hasStarted() && !hasEnded();
        }
        
        
        virtual void addChild( Ptr<IPartTracker> const& child ) CATCH_OVERRIDE {
            m_children.push_back( child );
            size_t childCount = m_children.size();
        }
        
        virtual IPartTracker* findChild( std::string const& name ) CATCH_OVERRIDE {
            Children::const_iterator it = std::find_if( m_children.begin(), m_children.end(), TrackerHasName( name ) );
            return( it != m_children.end() )
                ? it->get()
                : CATCH_NULL;
        }
        virtual IPartTracker& parent() CATCH_OVERRIDE {
            assert( m_parent ); // Should always be non-null except for root
            return *m_parent;
        }
        
        virtual void openChild() CATCH_OVERRIDE {
            if( m_runState != ExecutingChildren ) {
                m_runState = ExecutingChildren;
                if( m_parent )
                    m_parent->openChild();
            }
        }
        void open() {
            m_runState = Executing;
            moveToThis();
            if( m_parent )
                m_parent->openChild();
        }
        
        virtual void close() CATCH_OVERRIDE {
            
            // Close any still open children (e.g. generators)
            while( &m_ctx.currentPart() != this )
                m_ctx.currentPart().close();

            switch( m_runState ) {
                case CompletedSuccessfully:
                case Failed:
                    return;

                case Executing:
                    m_runState = CompletedSuccessfully;
                    break;
                case ExecutingChildren:
                    if( m_children.empty() || m_children.back()->hasEnded() )
                        m_runState = CompletedSuccessfully;
                    break;
                case NeedsAnotherRun:
                    m_runState = Executing;
                    break;
                default:
                    throw std::logic_error( "Unexpected state" );
            }
            moveToParent();
            m_ctx.completeCycle();
        }
        virtual void fail() CATCH_OVERRIDE {
            m_runState = Failed;
            if( m_parent )
                m_parent->markAsNeedingAnotherRun();
            moveToParent();
            m_ctx.completeCycle();
        }
        virtual void markAsNeedingAnotherRun() CATCH_OVERRIDE {
            m_runState = NeedsAnotherRun;
        }
    private:
        void moveToParent() {
            assert( m_parent );
            m_ctx.setCurrentPart( m_parent );
        }
        void moveToThis() {
            m_ctx.setCurrentPart( this );
        }
    };
    


    class SectionTracker : public PartTrackerBase {
    public:
        SectionTracker( std::string const& name, TrackerContext& ctx, IPartTracker* parent )
        :   PartTrackerBase( name, ctx, parent )
        {}
        
        static SectionTracker& acquire( TrackerContext& ctx, std::string const& name ) {
            SectionTracker* section = CATCH_NULL;
            
            IPartTracker& currentPart = ctx.currentPart();
            if( IPartTracker* part = currentPart.findChild( name ) ) {
                section = dynamic_cast<SectionTracker*>( part );
                assert( section );
            }
            else {
                section = new SectionTracker( name, ctx, &currentPart );
                currentPart.addChild( section );
            }
            if( !ctx.completedCycle() && !section->hasEnded() ) {
                
                section->open();
            }
            return *section;
        }
    };
    
    class IndexTracker : public PartTrackerBase {
        int m_size;
        int m_index;
    public:
        IndexTracker( std::string const& name, TrackerContext& ctx, IPartTracker* parent, int size )
        :   PartTrackerBase( name, ctx, parent ),
            m_size( size ),
            m_index( -1 )
        {}
        
        static IndexTracker& acquire( TrackerContext& ctx, std::string const& name, int size ) {
            IndexTracker* tracker = CATCH_NULL;
            
            IPartTracker& currentPart = ctx.currentPart();
            if( IPartTracker* part = currentPart.findChild( name ) ) {
                tracker = dynamic_cast<IndexTracker*>( part );
                assert( tracker );
            }
            else {
                tracker = new IndexTracker( name, ctx, &currentPart, size );
                currentPart.addChild( tracker );
            }
            
            if( !ctx.completedCycle() && !tracker->hasEnded() ) {
                if( tracker->m_runState != ExecutingChildren )
                    tracker->moveNext();
                tracker->open();
            }
            
            return *tracker;
        }
        
        int index() const { return m_index; }
        
        void moveNext() {
            m_index++;
            m_children.clear();
        }
        
        virtual void close() CATCH_OVERRIDE {
            PartTrackerBase::close();
            if( m_runState == CompletedSuccessfully )
                if( m_index < m_size-1 )
                    m_runState = Executing;
        }
    };
    
    IPartTracker& TrackerContext::startRun() {
        m_rootPart = new SectionTracker( "{root}", *this, CATCH_NULL );
        m_currentPart = CATCH_NULL;
        m_runState = Executing;
        return *m_rootPart;
    }
    
    class LocalContext {
        
    public:
        TrackerContext& operator()() const {
            return TrackerContext::instance();
        }
    };
    
} // namespace Catch

inline Catch::TrackerContext& C_A_T_C_H_Context() {
    return Catch::TrackerContext::instance();
}

// -------------------

#include "catch.hpp"

using namespace Catch;

inline void testCase( Catch::LocalContext const& C_A_T_C_H_Context ) {

//    REQUIRE( C_A_T_C_H_Context().i() == 42 );
}

TEST_CASE( "PartTracker" ) {
    
    TrackerContext ctx;
    ctx.startRun();
    ctx.startCycle();
    
    IPartTracker& testCase = SectionTracker::acquire( ctx, "Testcase" );
    REQUIRE( testCase.isSuccessfullyCompleted() == false );

    IPartTracker& s1 = SectionTracker::acquire( ctx, "S1" );
    REQUIRE( s1.isOpen() == true );
    REQUIRE( s1.isSuccessfullyCompleted() == false );

    SECTION( "successfully close one section" ) {
        s1.close();
        REQUIRE( s1.isSuccessfullyCompleted() == true );
        REQUIRE( testCase.hasEnded() == false );

        testCase.close();
        REQUIRE( ctx.completedCycle() == true );
        REQUIRE( testCase.isSuccessfullyCompleted() == true );
    }
    
    SECTION( "fail one section" ) {
        s1.fail();
        REQUIRE( s1.isSuccessfullyCompleted() == false );
        REQUIRE( s1.hasEnded() == true );
        REQUIRE( testCase.isSuccessfullyCompleted() == false );
        REQUIRE( testCase.hasEnded() == false );
        
        testCase.close();
        REQUIRE( ctx.completedCycle() == true );
        REQUIRE( testCase.isSuccessfullyCompleted() == false );

        SECTION( "re-enter after failed section" ) {
            ctx.startCycle();
            IPartTracker& testCase2 = SectionTracker::acquire( ctx, "Testcase" );
            REQUIRE( testCase2.isSuccessfullyCompleted() == false );
            
            IPartTracker& s1b = SectionTracker::acquire( ctx, "S1" );
            REQUIRE( s1b.isOpen() == false );
            
            testCase2.close();
            REQUIRE( ctx.completedCycle() == true );
            REQUIRE( testCase.isSuccessfullyCompleted() == true );
            REQUIRE( testCase.hasEnded() == true );
        }
        SECTION( "re-enter after failed section and find next section" ) {
            ctx.startCycle();
            IPartTracker& testCase2 = SectionTracker::acquire( ctx, "Testcase" );
            REQUIRE( testCase2.isSuccessfullyCompleted() == false );
            
            IPartTracker& s1b = SectionTracker::acquire( ctx, "S1" );
            REQUIRE( s1b.isSuccessfullyCompleted() == false );

            IPartTracker& s2 = SectionTracker::acquire( ctx, "S2" );
            REQUIRE( s2.isOpen() );
            s2.close();
            REQUIRE( ctx.completedCycle() == true );
            
            testCase2.close();
            REQUIRE( testCase.isSuccessfullyCompleted() == true );
            REQUIRE( testCase.hasEnded() == true );
        }
    }
    
    SECTION( "successfully close one section, then find another" ) {
        s1.close();
        REQUIRE( ctx.completedCycle() == true );
        
        IPartTracker& s2 = SectionTracker::acquire( ctx, "S2" );
        REQUIRE( s2.isOpen() == false );
        REQUIRE( s2.isSuccessfullyCompleted() == false );
        
        testCase.close();
        REQUIRE( testCase.isSuccessfullyCompleted() == false );

        SECTION( "Re-enter - skips S1 and enters S2" ) {
            ctx.startCycle();
            IPartTracker& testCase2 = SectionTracker::acquire( ctx, "Testcase" );
            REQUIRE( testCase2.isSuccessfullyCompleted() == false );
            REQUIRE( testCase2.isSuccessfullyCompleted() == false );
            
            IPartTracker& s1b = SectionTracker::acquire( ctx, "S1" );
            REQUIRE( s1b.isOpen() == false );

            IPartTracker& s2b = SectionTracker::acquire( ctx, "S2" );
            REQUIRE( s2b.isOpen() );
            REQUIRE( s2b.isSuccessfullyCompleted() == false );

            REQUIRE( ctx.completedCycle() == false );
            
            SECTION ("Successfully close S2") {
                s2b.close();
                REQUIRE( ctx.completedCycle() == true );

                REQUIRE( s2b.isSuccessfullyCompleted() == true );
                REQUIRE( testCase2.hasEnded() == false );
                
                testCase2.close();
                REQUIRE( testCase2.isSuccessfullyCompleted() == true );
            }
            SECTION ("fail S2") {
                s2b.fail();
                REQUIRE( ctx.completedCycle() == true );

                REQUIRE( s2b.isSuccessfullyCompleted() == false );
                REQUIRE( s2b.hasEnded() == true );
                REQUIRE( testCase2.hasEnded() == false );
                
                testCase2.close();
                REQUIRE( testCase2.isSuccessfullyCompleted() == false );
            }
        }
    }
    
    SECTION( "open a nested section" ) {
        IPartTracker& s2 = SectionTracker::acquire( ctx, "S2" );
        REQUIRE( s2.isOpen() == true );

        s2.close();
        REQUIRE( s2.isSuccessfullyCompleted() == true );
        REQUIRE( s1.isSuccessfullyCompleted() == false );
        
        s1.close();
        REQUIRE( s1.isSuccessfullyCompleted() == true );
        REQUIRE( testCase.isSuccessfullyCompleted() == false );
        
        testCase.close();
        REQUIRE( testCase.isSuccessfullyCompleted() == true );
    }
    
    SECTION( "start a generator" ) {
        IndexTracker& g1 = IndexTracker::acquire( ctx, "G1", 2 );
        REQUIRE( g1.isOpen() == true );
        REQUIRE( g1.index() == 0 );

        REQUIRE( g1.isSuccessfullyCompleted() == false );
        REQUIRE( s1.isSuccessfullyCompleted() == false );

        SECTION( "close outer section" )
        {
            s1.close();
            REQUIRE( s1.isSuccessfullyCompleted() == false );
            testCase.close();
            REQUIRE( testCase.isSuccessfullyCompleted() == false );

            SECTION( "Re-enter for second generation" ) {
                ctx.startCycle();
                IPartTracker& testCase2 = SectionTracker::acquire( ctx, "Testcase" );
                REQUIRE( testCase2.isOpen() == true );
                
                IPartTracker& s1b = SectionTracker::acquire( ctx, "S1" );
                REQUIRE( s1b.isOpen() == true );
                
                
                IndexTracker& g1b = IndexTracker::acquire( ctx, "G1", 2 );
                REQUIRE( g1b.isOpen() == true );
                REQUIRE( g1b.index() == 1 );
                
                REQUIRE( s1.isSuccessfullyCompleted() == false );
                
                s1b.close();
                REQUIRE( s1b.isSuccessfullyCompleted() == true );
                REQUIRE( g1b.isSuccessfullyCompleted() == true );
                testCase2.close();
                REQUIRE( testCase2.isSuccessfullyCompleted() == true );
            }
        }
        SECTION( "Start a new inner section" ) {
            IPartTracker& s2 = SectionTracker::acquire( ctx, "S2" );
            REQUIRE( s2.isOpen() == true );

            s2.close();
            REQUIRE( s2.isSuccessfullyCompleted() == true );

            s1.close();
            REQUIRE( s1.isSuccessfullyCompleted() == false );

            testCase.close();
            REQUIRE( testCase.isSuccessfullyCompleted() == false );
            
            SECTION( "Re-enter for second generation" ) {
                ctx.startCycle();
                IPartTracker& testCase2 = SectionTracker::acquire( ctx, "Testcase" );
                REQUIRE( testCase2.isSuccessfullyCompleted() == false );
                
                IPartTracker& s1b = SectionTracker::acquire( ctx, "S1" );
                REQUIRE( s1b.isSuccessfullyCompleted() == false );
                
                // generator - next value
                IndexTracker& g1b = IndexTracker::acquire( ctx, "G1", 2 );
                REQUIRE( g1b.isOpen() == true );
                REQUIRE( g1b.index() == 1 );
                
                // inner section again
                IPartTracker& s2b = SectionTracker::acquire( ctx, "S2" );
                REQUIRE( s2b.isOpen() == true );
                
                s2b.close();
                REQUIRE( s2b.isSuccessfullyCompleted() == true );
                
                s1b.close();
                REQUIRE( s1b.isSuccessfullyCompleted() == true );
                REQUIRE( g1b.isSuccessfullyCompleted() == true );
                
                testCase2.close();
                REQUIRE( testCase2.isSuccessfullyCompleted() == true );
            }
        }
        // !TBD"
        //   nested generator
        //   two sections within a generator
    }
}
